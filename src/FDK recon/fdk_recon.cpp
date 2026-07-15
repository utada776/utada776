// ============================================================================
// fdk_recon.cpp
//
// RTK-backed FDK（Feldkamp-Davis-Kress）锥形束 CT 重建引擎实现。
//
// ── 算法总流程（FDK 滤波反投影）──────────────────────────────────────────
//   ① 投影预处理：增益归一化 + 对数变换 p = -ln(I/I₀) + 准直裁剪
//   ② 将 ProjectionImage 序列打包为 RTK/ITK 3D projection stack
//   ③ 构造 rtk::ThreeDCircularProjectionGeometry
//   ④ RTK pipeline: displaced detector weighting / Parker / ramp / backprojection
//   ⑤ 后处理：输出 HU 映射与可选截断
//
// 算法参考：
//   Feldkamp L.A., Davis L.C., Kress J.W. (1984)
//   "Practical cone-beam algorithm", J. Opt. Soc. Am. A, 1(6), 612-619.
// ============================================================================

#include "FDK recon/fdk_recon.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef FDK_HAS_RTK
#include <itkCommand.h>
#include <itkImage.h>
#include <itkMacro.h>
#include <rtkDisplacedDetectorImageFilter.h>
#include <rtkDisplacedDetectorForOffsetFieldOfViewImageFilter.h>
#include <rtkElektaXVI5GeometryXMLFileReader.h>
#include <rtkFDKConeBeamReconstructionFilter.h>
#include <rtkFieldOfViewImageFilter.h>
#include <rtkParkerShortScanImageFilter.h>
#include <rtkThreeDCircularProjectionGeometry.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace fdk {

// ============================================================================
// 字符串工具函数
// ============================================================================

/// 去除字符串首尾空白符（空格、制表符、回车、换行）
static std::string TrimString(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

/// 将字符串转换为全小写（用于大小写无关比较）
static std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ============================================================================
// IniConfig 实现
// ============================================================================

bool IniConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string current_section;
    std::string line;
    while (std::getline(f, line)) {
        line = TrimString(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const auto end = line.find(']');
            if (end != std::string::npos) {
                current_section = TrimString(line.substr(1, end - 1));
            }
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimString(line.substr(0, eq));
        const std::string val = TrimString(line.substr(eq + 1));
        if (key.empty()) continue;

        // Find or create section
        bool found_section = false;
        for (auto& sec : m_sections) {
            if (ToLower(sec.first) == ToLower(current_section)) {
                sec.second.push_back({key, val});
                found_section = true;
                break;
            }
        }
        if (!found_section) {
            m_sections.push_back({current_section, {{key, val}}});
        }
    }
    return true;
}

std::string IniConfig::get_string(const std::string& section, const std::string& key,
                                   const std::string& default_val) const {
    const std::string sec_lower = ToLower(section);
    const std::string key_lower = ToLower(key);
    for (const auto& sec : m_sections) {
        if (ToLower(sec.first) == sec_lower) {
            for (const auto& kv : sec.second) {
                if (ToLower(kv.first) == key_lower) return kv.second;
            }
        }
    }
    return default_val;
}

int IniConfig::get_int(const std::string& section, const std::string& key, int default_val) const {
    const std::string v = get_string(section, key);
    if (v.empty()) return default_val;
    try { return std::stoi(v); } catch (...) { return default_val; }
}

float IniConfig::get_float(const std::string& section, const std::string& key, float default_val) const {
    const std::string v = get_string(section, key);
    if (v.empty()) return default_val;
    try { return std::stof(v); } catch (...) { return default_val; }
}

bool IniConfig::get_bool(const std::string& section, const std::string& key, bool default_val) const {
    std::string v = ToLower(TrimString(get_string(section, key)));
    if (v.empty()) return default_val;
    if (v == "1" || v == "true" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "no") return false;
    return default_val;
}

void IniConfig::apply_to_params(FdkParams& p) const {
    // 每个参数先检查 ini 中是否存在有效值，存在才覆盖，保留调用方默认值。
    const std::string sec = "RECONSTRUCTION";

    const int nx = get_int(sec, "DimX", 0);
    if (nx > 0) p.nx = nx;
    const int ny = get_int(sec, "DimY", 0);
    if (ny > 0) p.ny = ny;
    const int nz = get_int(sec, "DimZ", 0);
    if (nz > 0) p.nz = nz;

    const float vs = get_float(sec, "VoxelSize", 0.0f);
    if (vs > 0.0f) p.voxel_size_cm = vs;

    const float offset_x = get_float(sec, "ReconstructionOffsetX", get_float(sec, "OffsetX", p.offset_x_cm));
    p.offset_x_cm = offset_x;
    const float offset_y = get_float(sec, "ReconstructionOffsetY", get_float(sec, "OffsetY", p.offset_y_cm));
    p.offset_y_cm = offset_y;
    const float offset_z = get_float(sec, "ReconstructionOffsetZ", get_float(sec, "OffsetZ", p.offset_z_cm));
    p.offset_z_cm = offset_z;

    const float detector_offset_u_scale = get_float(sec, "DetectorOffsetUScale", p.detector_offset_u_scale);
    if (detector_offset_u_scale > 0.0f) p.detector_offset_u_scale = detector_offset_u_scale;
    const float detector_offset_v_scale = get_float(sec, "DetectorOffsetVScale", p.detector_offset_v_scale);
    if (detector_offset_v_scale > 0.0f) p.detector_offset_v_scale = detector_offset_v_scale;

    const float fdd = get_float(sec, "FDD", 0.0f);
    if (fdd > 0.0f) p.fdd_cm = fdd;

    const float fid = get_float(sec, "FID", 0.0f);
    if (fid > 0.0f) p.fid_cm = fid;

    const float start = get_float(sec, "GantryStartAngle", -9999.0f);
    if (start > -9000.0f) p.gantry_start_angle_deg = start;

    const float stop = get_float(sec, "GantryStopAngle", -9999.0f);
    if (stop > -9000.0f) p.gantry_stop_angle_deg = stop;

    const float cone = get_float(sec, "ConeAngle", 0.0f);
    if (cone > 0.0f) p.cone_angle_deg = cone;

    const std::string filt = get_string(sec, "Filter");
    if (!filt.empty()) p.filter = filt;

    const float fa = get_float(sec, "FilterParam", -1.0f);
    if (fa >= 0.0f) p.filter_param_a = fa;

    const float snr = get_float(sec, "FilterSNR", 0.0f);
    if (snr > 0.0f) p.filter_snr = snr;

    const bool ss = get_bool(sec, "ShortScan", false);
    p.short_scan = ss;

    const bool wbp = get_bool(sec, "WeightBP", p.weight_bp);
    p.weight_bp = wbp;

    const float wmin = get_float(sec, "AngleWeightMinDeg", -1.0f);
    if (wmin > 0.0f) p.angle_weight_min_deg = wmin;

    const float wmax = get_float(sec, "AngleWeightMaxDeg", -1.0f);
    if (wmax > 0.0f) p.angle_weight_max_deg = wmax;

    const int projection_edge_taper_pixels = get_int(sec, "ProjectionEdgeTaperPixels", p.projection_edge_taper_pixels);
    if (projection_edge_taper_pixels >= 0) p.projection_edge_taper_pixels = projection_edge_taper_pixels;

    const std::string custom_half_fan_weight = get_string(sec, "CustomHalfFanWeight");
    if (!custom_half_fan_weight.empty()) p.custom_half_fan_weight = custom_half_fan_weight;
    const float custom_half_fan_width = get_float(sec, "CustomHalfFanWidthPixels", p.custom_half_fan_width_pixels);
    if (custom_half_fan_width >= 0.0f) p.custom_half_fan_width_pixels = custom_half_fan_width;
    const float custom_half_fan_strength = get_float(sec, "CustomHalfFanStrength", p.custom_half_fan_strength);
    if (custom_half_fan_strength >= 0.0f) p.custom_half_fan_strength = custom_half_fan_strength;

    p.rtk_transpose_projections = get_bool(sec, "RtkTransposeProjections", p.rtk_transpose_projections);
    const std::string rtk_explicit_geometry_axis = get_string(sec, "RtkExplicitGeometryAxis");
    if (!rtk_explicit_geometry_axis.empty()) p.rtk_explicit_geometry_axis = rtk_explicit_geometry_axis;
    const std::string rtk_explicit_row_axis = get_string(sec, "RtkExplicitRowAxis");
    if (!rtk_explicit_row_axis.empty()) p.rtk_explicit_row_axis = rtk_explicit_row_axis;
    const std::string rtk_explicit_col_axis = get_string(sec, "RtkExplicitColAxis");
    if (!rtk_explicit_col_axis.empty()) p.rtk_explicit_col_axis = rtk_explicit_col_axis;

    const std::string scale_text = get_string(sec, "ScaleOut");
    if (!scale_text.empty()) {
        try { p.scale_out = std::stof(scale_text); } catch (...) {}
    }

    const std::string offset_text = get_string(sec, "OffsetOut");
    if (!offset_text.empty()) {
        try { p.offset_out = std::stof(offset_text); } catch (...) {}
    }

    const float air = get_float(sec, "AirValue", 0.0f);
    if (air > 0.0f) p.air_value = air;

    const std::string gain = get_string(sec, "GainFile");
    if (!gain.empty()) p.gain_file = gain;

    const float bh = get_float(sec, "BeamHardening", -1.0f);
    if (bh >= 0.0f) p.beam_hardening_coeff = bh;

    const float sg = get_float(sec, "ScatterSigma", -1.0f);
    if (sg >= 0.0f) p.scatter_sigma_cm = sg;

    const int thr = get_int(sec, "NumThreads", -1);
    if (thr >= 0) p.num_threads = thr;

    // Collimator skip
    const int scl = get_int(sec, "SkipColumnsLeft", -1);
    if (scl >= 0) p.skip_cols_left = scl;
    const int scr = get_int(sec, "SkipColumnsRight", -1);
    if (scr >= 0) p.skip_cols_right = scr;
    const int srt = get_int(sec, "SkipRowsTop", -1);
    if (srt >= 0) p.skip_rows_top = srt;
    const int srb = get_int(sec, "SkipRowsBottom", -1);
    if (srb >= 0) p.skip_rows_bottom = srb;

    // Detector dimensions (critical for HIS file header detection)
    const int dcols = get_int(sec, "DetectorCols", 0);
    if (dcols > 0) p.detector_cols = dcols;
    const int drows = get_int(sec, "DetectorRows", 0);
    if (drows > 0) p.detector_rows = drows;
    const float dsize = get_float(sec, "DetectorSizeCm", 0.0f);
    if (dsize > 0.0f) p.detector_size_cm = dsize;
}

// ============================================================================
// 帧角度信息加载
// ============================================================================

/// 辅助函数：从单行字符串中提取已知 XML 标签的内层文本。
/// 例："<GantryAngle>90.5</GantryAngle>" → value = "90.5"
static bool ExtractXmlTagValue(const std::string& line,
                                const std::string& tag,
                                std::string& value) {
    const std::string open  = "<"  + tag + ">";
    const std::string close = "</" + tag + ">";
    const std::size_t p = line.find(open);
    if (p == std::string::npos) return false;
    const std::size_t start = p + open.size();
    const std::size_t end   = line.find(close, start);
    if (end == std::string::npos) return false;
    value = line.substr(start, end - start);
    return true;
}

static bool LoadFrameInfoXml(const std::string& path,
                              std::vector<FrameInfo>& out,
                              std::string& error) {
    std::ifstream f(path);
    if (!f.is_open()) {
        error = "Cannot open frame info XML: " + path;
        return false;
    }
    
    std::string line;
    int auto_id = 0;
    int xml_detector_cols = 512;
    int xml_detector_rows = 512;
    bool in_frame = false;
    bool have_angle = false;
    FrameInfo current_frame;
    
    while (std::getline(f, line)) {
        std::string val;
        
        // Read detector dimensions from <Image> section
        if (ExtractXmlTagValue(line, "Width", val)) {
            try { xml_detector_cols = std::stoi(val); } catch (...) {}
        }
        if (ExtractXmlTagValue(line, "Height", val)) {
            try { xml_detector_rows = std::stoi(val); } catch (...) {}
        }
        
        if (line.find("<Frame>") != std::string::npos) {
            in_frame = true;
            have_angle = false;
            current_frame = FrameInfo{};
            current_frame.frame_id = auto_id;
            current_frame.detector_cols = xml_detector_cols;
            current_frame.detector_rows = xml_detector_rows;
        }

        if (in_frame && ExtractXmlTagValue(line, "Seq", val)) {
            try { current_frame.frame_id = std::max(0, std::stoi(val) - 1); } catch (...) {}
        }

        // Read per-frame detector centre offsets.
        if (ExtractXmlTagValue(line, "UCentre", val)) {
            try { current_frame.u_centre = std::stof(val); } catch (...) {}
        }
        if (ExtractXmlTagValue(line, "VCentre", val)) {
            try { current_frame.v_centre = std::stof(val); } catch (...) {}
        }
        
        if (in_frame && ExtractXmlTagValue(line, "GantryAngle", val)) {
            try {
                current_frame.gantry_angle_deg = std::stof(val);
                have_angle = true;
            } catch (...) {}
        }

        if (in_frame && line.find("</Frame>") != std::string::npos) {
            if (have_angle) {
                current_frame.detector_cols = xml_detector_cols;
                current_frame.detector_rows = xml_detector_rows;
                out.push_back(current_frame);
                auto_id = std::max(auto_id + 1, current_frame.frame_id + 1);
            }
            in_frame = false;
            have_angle = false;
        }
    }
    if (out.empty()) {
        error = "Frame info XML has no <GantryAngle> entries: " + path;
        return false;
    }
    return true;
}

bool LoadFrameInfo(const std::string& path,
                   std::vector<FrameInfo>& out,
                   std::string& error) {
    // Dispatch XML files to dedicated parser
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".xml") {
            return LoadFrameInfoXml(path, out, error);
        }
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        error = "Cannot open frame info file: " + path;
        return false;
    }

    std::string line;
    int line_num = 0;
    int auto_id  = 0;
    while (std::getline(f, line)) {
        ++line_num;
        // Strip Windows-style CR
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = TrimString(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '/') continue;

        // Replace comma/tab/semicolon delimiters with space for uniform parsing
        for (char& c : line) {
            if (c == ',' || c == '\t' || c == ';') c = ' ';
        }

        std::istringstream iss(line);

        // Try to read first token — must be numeric (skip header lines like "FrameID Angle")
        std::string tok1, tok2;
        if (!(iss >> tok1)) continue;

        // Detect non-numeric header tokens (e.g. "FrameID", "Angle", "index")
        bool tok1_numeric = !tok1.empty() &&
            (std::isdigit(static_cast<unsigned char>(tok1[0])) ||
             tok1[0] == '-' || tok1[0] == '+' || tok1[0] == '.');
        if (!tok1_numeric) continue;  // silently skip header lines

        FrameInfo fi;
        if (iss >> tok2) {
            // Two tokens: either "frame_id  angle"  or  "angle  something_else"
            bool tok2_numeric = !tok2.empty() &&
                (std::isdigit(static_cast<unsigned char>(tok2[0])) ||
                 tok2[0] == '-' || tok2[0] == '+' || tok2[0] == '.');

            if (tok2_numeric) {
                // Standard format: frame_id  gantry_angle
                try {
                    fi.frame_id         = std::stoi(tok1);
                    fi.gantry_angle_deg = std::stof(tok2);
                } catch (...) {
                    continue;  // skip malformed line
                }
            } else {
                // First token is angle, second is not numeric — treat as angle-only
                try { fi.gantry_angle_deg = std::stof(tok1); } catch (...) { continue; }
                fi.frame_id = auto_id++;
            }
        } else {
            // Single token: just an angle
            try { fi.gantry_angle_deg = std::stof(tok1); } catch (...) { continue; }
            fi.frame_id = auto_id++;
        }
        out.push_back(fi);
    }

    if (out.empty()) {
        error = "Frame info file is empty or has no valid entries: " + path;
        return false;
    }
    return true;
}

// ============================================================================
// HIS 图像读取器
//
// HIS（Heimann Imaging System）是 Perkin-Elmer 平板探测器的原始格式。
// 文件结构：可选 68 字节文件头（首 2 字节 0x0000 为标记）+ uint16 像素数据。
// 本实现自动检测文件头是否存在，无论有无头均可正确读取。
// ============================================================================
static bool ReadHisImage(const std::string& path, int expected_w, int expected_h,
                          std::vector<float>& pixels_out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) { err = "Cannot open: " + path; return false; }

    f.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    std::size_t header_bytes = 0;
    if (expected_w > 0 && expected_h > 0) {
        const std::size_t expected_bytes =
            static_cast<std::size_t>(expected_w) * static_cast<std::size_t>(expected_h) * sizeof(std::uint16_t);
        for (const std::size_t candidate : {std::size_t(0), std::size_t(68), std::size_t(100)}) {
            if (file_size == expected_bytes + candidate) {
                header_bytes = candidate;
                break;
            }
        }
    }
    if (header_bytes == 0) {
        // Fallback heuristic for older files: standard Perkin-Elmer HIS header is often 68 bytes.
        std::uint16_t marker = 0;
        f.read(reinterpret_cast<char*>(&marker), 2);
        f.seekg(0);
        if (marker == 0x0000 && file_size >= 68) {
            header_bytes = 68;
        }
    }

    const std::size_t data_bytes = file_size - header_bytes;
    const std::size_t n_pixels   = data_bytes / sizeof(std::uint16_t);

    const char* debug_his = std::getenv("FDK_DEBUG_HIS");
    if (debug_his && std::string(debug_his) == "1") {
        std::cout << "[HIS] File: " << path << std::endl;
        std::cout << "[HIS] File size: " << file_size << " bytes" << std::endl;
        std::cout << "[HIS] Detected header: " << header_bytes << " bytes" << std::endl;
        std::cout << "[HIS] Expected dimensions: " << expected_w << " x " << expected_h << std::endl;
        std::cout << "[HIS] Pixel data: " << data_bytes << " bytes = " << n_pixels << " pixels" << std::endl;
    }

    if (expected_w > 0 && expected_h > 0) {
        const std::size_t expected = static_cast<std::size_t>(expected_w * expected_h);
        if (n_pixels != expected) {
            err = "HIS pixel count mismatch in " + path +
                  " (expected " + std::to_string(expected) +
                  ", got " + std::to_string(n_pixels) + ")";
            return false;
        }
    }

    f.seekg(static_cast<std::streamoff>(header_bytes));
    std::vector<std::uint16_t> raw(n_pixels);
    f.read(reinterpret_cast<char*>(raw.data()),
           static_cast<std::streamsize>(n_pixels * sizeof(std::uint16_t)));

    pixels_out.resize(n_pixels);
    for (std::size_t i = 0; i < n_pixels; ++i) {
        pixels_out[i] = static_cast<float>(raw[i]);
    }
    return true;
}

static float EstimateRawAirValue(const std::vector<float>& raw_pixels, float fallback) {
    if (raw_pixels.empty()) {
        return fallback;
    }

    std::vector<float> positive;
    positive.reserve(raw_pixels.size());
    for (float v : raw_pixels) {
        if (v > 1000.0f && std::isfinite(v)) {
            positive.push_back(v);
        }
    }
    if (positive.empty()) {
        return fallback;
    }

    const std::size_t idx = static_cast<std::size_t>(
        std::max<double>(0.0, std::min<double>(positive.size() - 1,
            std::floor(0.995 * static_cast<double>(positive.size() - 1)))));
    std::nth_element(positive.begin(), positive.begin() + idx, positive.end());
    const float p995 = positive[idx];
    return (p995 > 1000.0f) ? p995 : fallback;
}

static float EstimatePositivePercentile(const std::vector<float>& pixels, double percentile, float fallback) {
    if (pixels.empty()) return fallback;
    std::vector<float> values;
    values.reserve(pixels.size());
    for (float value : pixels) {
        if (value > 1000.0f && std::isfinite(value)) values.push_back(value);
    }
    if (values.empty()) return fallback;
    percentile = std::max(0.0, std::min(1.0, percentile));
    const std::size_t idx = static_cast<std::size_t>(
        std::floor(percentile * static_cast<double>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return values[idx];
}

static double EnvDouble(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
}

static int EnvInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

static bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    const std::string text = ToLower(TrimString(value));
    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;
    return fallback;
}

static int AxisIndexFromChar(char axis) {
    axis = static_cast<char>(std::tolower(static_cast<unsigned char>(axis)));
    if (axis == 'x' || axis == '0') return 0;
    if (axis == 'y' || axis == '1') return 1;
    if (axis == 'z' || axis == '2') return 2;
    return -1;
}

static bool ParseVolumeDirectionAxes(const char* text, int axes[3]) {
    if (!text || std::strlen(text) != 3) return false;
    bool seen[3] = {false, false, false};
    for (int i = 0; i < 3; ++i) {
        axes[i] = AxisIndexFromChar(text[i]);
        if (axes[i] < 0 || seen[axes[i]]) return false;
        seen[axes[i]] = true;
    }
    return true;
}

static bool ParseVolumeDirectionSigns(const char* text, double signs[3]) {
    if (!text || std::strlen(text) != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (text[i] == '+') signs[i] = 1.0;
        else if (text[i] == '-') signs[i] = -1.0;
        else return false;
    }
    return true;
}

static bool ParseVolumeDirectionSignsInt(const char* text, int signs[3]) {
    double double_signs[3] = {1.0, 1.0, 1.0};
    if (!ParseVolumeDirectionSigns(text, double_signs)) return false;
    for (int i = 0; i < 3; ++i) signs[i] = (double_signs[i] < 0.0) ? -1 : 1;
    return true;
}

static bool ApplyOutputReorientationIfRequested(Volume3D& volume) {
    const char* axes_text = std::getenv("FDK_OUTPUT_REORIENT_AXES");
    if (!axes_text || !*axes_text || !volume.valid()) return false;

    int axes[3] = {0, 1, 2};
    int signs[3] = {1, 1, 1};
    if (!ParseVolumeDirectionAxes(axes_text, axes)) return false;
    ParseVolumeDirectionSignsInt(std::getenv("FDK_OUTPUT_REORIENT_SIGNS"), signs);

    const int input_dims[3] = {volume.nx, volume.ny, volume.nz};
    const int output_dims[3] = {input_dims[axes[0]], input_dims[axes[1]], input_dims[axes[2]]};
    std::vector<float> output(static_cast<std::size_t>(output_dims[0]) *
                              static_cast<std::size_t>(output_dims[1]) *
                              static_cast<std::size_t>(output_dims[2]), 0.0f);

    const auto input_index = [&](int x, int y, int z) -> std::size_t {
        return (static_cast<std::size_t>(z) * volume.ny + y) * volume.nx + x;
    };
    const auto output_index = [&](int x, int y, int z) -> std::size_t {
        return (static_cast<std::size_t>(z) * output_dims[1] + y) * output_dims[0] + x;
    };

    for (int oz = 0; oz < output_dims[2]; ++oz) {
        for (int oy = 0; oy < output_dims[1]; ++oy) {
            for (int ox = 0; ox < output_dims[0]; ++ox) {
                const int out_coord[3] = {ox, oy, oz};
                int in_coord[3] = {0, 0, 0};
                for (int out_axis = 0; out_axis < 3; ++out_axis) {
                    const int in_axis = axes[out_axis];
                    const int dim = input_dims[in_axis];
                    in_coord[in_axis] = (signs[out_axis] > 0)
                        ? out_coord[out_axis]
                        : (dim - 1 - out_coord[out_axis]);
                }
                output[output_index(ox, oy, oz)] =
                    volume.data[input_index(in_coord[0], in_coord[1], in_coord[2])];
            }
        }
    }

    volume.nx = output_dims[0];
    volume.ny = output_dims[1];
    volume.nz = output_dims[2];
    volume.data.swap(output);
    return true;
}

static void TransposeSquareProjectionPixels(std::vector<float>& pixels, int width, int height) {
    if (width <= 0 || height <= 0 || width != height) return;
    for (int y = 0; y < height; ++y) {
        for (int x = y + 1; x < width; ++x) {
            std::swap(pixels[static_cast<std::size_t>(y) * width + x],
                      pixels[static_cast<std::size_t>(x) * width + y]);
        }
    }
}

static void FlipProjectionPixelsU(std::vector<float>& pixels, int width, int height) {
    if (width <= 0 || height <= 0) return;
    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width / 2; ++x) {
            std::swap(pixels[row + x], pixels[row + (width - 1 - x)]);
        }
    }
}

static void FlipProjectionPixelsV(std::vector<float>& pixels, int width, int height) {
    if (width <= 0 || height <= 0) return;
    for (int y = 0; y < height / 2; ++y) {
        const std::size_t row_top = static_cast<std::size_t>(y) * width;
        const std::size_t row_bottom = static_cast<std::size_t>(height - 1 - y) * width;
        for (int x = 0; x < width; ++x) {
            std::swap(pixels[row_top + x], pixels[row_bottom + x]);
        }
    }
}

static void ShiftProjectionPixels(std::vector<float>& pixels, int width, int height, double shift_u, double shift_v) {
    if (width <= 0 || height <= 0 || pixels.empty() || (std::fabs(shift_u) < 1.0e-6 && std::fabs(shift_v) < 1.0e-6)) return;

    const std::vector<float> input = pixels;
    std::fill(pixels.begin(), pixels.end(), 0.0f);
    for (int y = 0; y < height; ++y) {
        const double src_y = static_cast<double>(y) - shift_v;
        const int y0 = static_cast<int>(std::floor(src_y));
        const int y1 = y0 + 1;
        if (y0 < 0 || y1 >= height) continue;
        const double fy = src_y - static_cast<double>(y0);
        const double wy0 = 1.0 - fy;
        const double wy1 = fy;
        for (int x = 0; x < width; ++x) {
            const double src_x = static_cast<double>(x) - shift_u;
            const int x0 = static_cast<int>(std::floor(src_x));
            const int x1 = x0 + 1;
            if (x0 < 0 || x1 >= width) continue;
            const double fx = src_x - static_cast<double>(x0);
            const double wx0 = 1.0 - fx;
            const double wx1 = fx;
            const std::size_t row0 = static_cast<std::size_t>(y0) * width;
            const std::size_t row1 = static_cast<std::size_t>(y1) * width;
            const double value =
                wx0 * wy0 * static_cast<double>(input[row0 + static_cast<std::size_t>(x0)]) +
                wx1 * wy0 * static_cast<double>(input[row0 + static_cast<std::size_t>(x1)]) +
                wx0 * wy1 * static_cast<double>(input[row1 + static_cast<std::size_t>(x0)]) +
                wx1 * wy1 * static_cast<double>(input[row1 + static_cast<std::size_t>(x1)]);
            pixels[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = static_cast<float>(value);
        }
    }
}

static void ApplyProjectionEdgeTaper(std::vector<float>& pixels, int width, int height, int taper_pixels) {
    if (width <= 0 || height <= 0 || taper_pixels <= 0) return;
    const int taper_x = std::min(taper_pixels, width / 2);
    const int taper_y = std::min(taper_pixels, height / 2);
    for (int y = 0; y < height; ++y) {
        double wy = 1.0;
        if (taper_y > 0) {
            if (y < taper_y) {
                wy = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(y) / static_cast<double>(taper_y));
            } else if (y >= height - taper_y) {
                wy = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(height - 1 - y) / static_cast<double>(taper_y));
            }
        }
        for (int x = 0; x < width; ++x) {
            double wx = 1.0;
            if (taper_x > 0) {
                if (x < taper_x) {
                    wx = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(x) / static_cast<double>(taper_x));
                } else if (x >= width - taper_x) {
                    wx = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(width - 1 - x) / static_cast<double>(taper_x));
                }
            }
            pixels[static_cast<std::size_t>(y) * width + x] *= static_cast<float>(wx * wy);
        }
    }
}

static void ApplyCustomHalfFanWeight(std::vector<float>& pixels,
                                     int width,
                                     int height,
                                     const std::string& mode,
                                     double width_pixels,
                                     double strength,
                                     double detector_pixel_cm,
                                     double detector_size_cm,
                                     double u_centre_cm,
                                     double fdd_cm) {
    if (width <= 0 || height <= 0 || pixels.empty() || mode.empty() || mode == "off" || mode == "0") return;

    width_pixels = std::max(1.0, width_pixels > 0.0 ? width_pixels : 0.5 * static_cast<double>(width));
    strength = std::max(0.0, std::min(1.0, strength));
    std::vector<float> weights(static_cast<std::size_t>(width), 1.0f);

    if (mode == "edge_left" || mode == "edge_right") {
        const int taper = static_cast<int>(std::min<double>(static_cast<double>(width), width_pixels));
        for (int x = 0; x < width; ++x) {
            const int d = (mode == "edge_left") ? x : (width - 1 - x);
            if (d < taper) {
                weights[static_cast<std::size_t>(x)] = static_cast<float>(0.5 - 0.5 * std::cos(M_PI * static_cast<double>(d) / static_cast<double>(std::max(1, taper))));
            }
        }
    } else if (mode == "wang_pos" || mode == "wang_neg") {
        const double center = 0.5 * static_cast<double>(width - 1);
        for (int x = 0; x < width; ++x) {
            const double t = std::max(-1.0, std::min(1.0, (static_cast<double>(x) - center) / width_pixels));
            const double s = std::sin(0.5 * M_PI * t);
            const double weight = (mode == "wang_pos") ? (1.0 + s) : (1.0 - s);
            weights[static_cast<std::size_t>(x)] = static_cast<float>(std::max(0.0, std::min(2.0, weight)));
        }
    } else if (mode == "elekta" || mode == "avl") {
        const double taper_margin_cm = std::max(0.0, EnvDouble("FDK_CUSTOM_HALF_FAN_TAPER_MARGIN_CM", 0.0));
        double overlap_length_cm = EnvDouble("FDK_CUSTOM_HALF_FAN_OVERLAP_CM", std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(overlap_length_cm) || overlap_length_cm <= 0.0) {
            overlap_length_cm = width_pixels * std::max(1.0e-6, detector_pixel_cm);
        }
        const double overlap_width_cm = detector_size_cm - 2.0 * std::fabs(u_centre_cm) - 2.0 * taper_margin_cm;
        const double half_overlap_cm = 0.5 * overlap_width_cm;
        if (half_overlap_cm <= 0.0 || fdd_cm <= 0.0 || detector_pixel_cm <= 0.0) return;

        const double detector_center = 0.5 * static_cast<double>(width - 1);
        const double sign = (u_centre_cm < 0.0) ? -1.0 : 1.0;
        if (!EnvFlag("FDK_CUSTOM_HALF_FAN_BEFORE_FILTER", true) && half_overlap_cm > 2.0 * overlap_length_cm) {
            const double dn = 2.0 * std::atan(overlap_length_cm / fdd_cm);
            for (int x = 0; x < width; ++x) {
                const double coord_cm = sign * (static_cast<double>(x) - detector_center) * detector_pixel_cm;
                double weight = 1.0;
                if (coord_cm > 0.0) {
                    if (coord_cm > half_overlap_cm) weight = 0.0;
                    else if (coord_cm < half_overlap_cm - overlap_length_cm) weight = 1.0;
                    else weight = 0.5 * std::cos(2.0 * M_PI * (std::atan(coord_cm / fdd_cm) -
                                      std::atan((half_overlap_cm - overlap_length_cm) / fdd_cm)) / dn) + 0.5;
                } else {
                    if (coord_cm < -half_overlap_cm) weight = 2.0;
                    else if (coord_cm > -half_overlap_cm + overlap_length_cm) weight = 1.0;
                    else weight = 0.5 * std::cos(2.0 * M_PI * (std::atan(coord_cm / fdd_cm) +
                                      std::atan((half_overlap_cm - overlap_length_cm) / fdd_cm)) / dn + M_PI) + 1.5;
                }
                weights[static_cast<std::size_t>(x)] = static_cast<float>(std::max(0.0, std::min(2.0, weight)));
            }
        } else {
            const double dn = 2.0 * std::atan(half_overlap_cm / fdd_cm);
            for (int x = 0; x < width; ++x) {
                const double coord_cm = sign * (static_cast<double>(x) - detector_center) * detector_pixel_cm;
                double weight = 1.0;
                if (coord_cm < -half_overlap_cm) weight = 2.0;
                if (std::fabs(coord_cm) < half_overlap_cm) {
                    weight = std::sin(M_PI * std::atan(-coord_cm / fdd_cm) / dn) + 1.0;
                }
                if (coord_cm >= half_overlap_cm) weight = 0.0;
                weights[static_cast<std::size_t>(x)] = static_cast<float>(std::max(0.0, std::min(2.0, weight)));
            }
        }
    } else {
        return;
    }

    if (strength < 1.0) {
        for (float& weight : weights) {
            weight = static_cast<float>(1.0 + strength * (static_cast<double>(weight) - 1.0));
        }
    }

    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            pixels[row + static_cast<std::size_t>(x)] *= weights[static_cast<std::size_t>(x)];
        }
    }
}

static void ApplyVirtualDetectorRecenter(ProjectionImage& projection,
                                         int virtual_width,
                                         double shift_pixels,
                                         double residual_u_centre_pixels) {
    const int width = projection.width;
    const int height = projection.height;
    if (width <= 0 || height <= 0 || virtual_width <= width || projection.pixels.empty()) return;

    std::vector<float> recentered(static_cast<std::size_t>(virtual_width) * height, 0.0f);
    const int dst_start = static_cast<int>(std::lround(
        0.5 * static_cast<double>(virtual_width - width) + shift_pixels));
    for (int y = 0; y < height; ++y) {
        const std::size_t src_row = static_cast<std::size_t>(y) * width;
        const std::size_t dst_row = static_cast<std::size_t>(y) * virtual_width;
        for (int x = 0; x < width; ++x) {
            const int dst_x = dst_start + x;
            if (dst_x >= 0 && dst_x < virtual_width) {
                recentered[dst_row + static_cast<std::size_t>(dst_x)] =
                    projection.pixels[src_row + static_cast<std::size_t>(x)];
            }
        }
    }

    projection.width = virtual_width;
    projection.u_centre = static_cast<float>(residual_u_centre_pixels);
    projection.pixels.swap(recentered);
}

// ============================================================================
// 投影图像批量加载
//
// 预处理管线（对每帧）：
//   1. 尝试多种文件命名约定（frame_XXXXXX.his / image_XXXXXX.his / ...）
//   2. 对数变换（Lambert-Beer 定律）：pix = -ln(pix / air_value)
//      将探测器计数转为射线路径积分（正比于衰减系数积分）
//   3. 准直裁剪：skip_* 区域像素清零，消除边缘伪影
// ============================================================================
bool LoadProjections(const FdkParams& params,
                     const std::vector<FrameInfo>& frames,
                     std::vector<ProjectionImage>& out,
                     std::function<void(int done, int total, const std::string& msg)> progress,
                     std::string& error) {
    namespace fs = std::filesystem;

    // Build a one-time index for Perkin-Elmer naming: 00001.*.his, 00002.*.his ...
    // This avoids O(N^2) directory scans and significantly speeds up startup.
    std::unordered_map<int, std::string> pe_his_by_seq;
    try {
        for (const auto& entry : fs::directory_iterator(params.image_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            if (filename.size() < 11) continue;
            if (filename[5] != '.') continue;
            if (filename.substr(filename.size() - 4) != ".his") continue;

            bool first5_all_digits = true;
            for (int d = 0; d < 5; ++d) {
                if (!std::isdigit(static_cast<unsigned char>(filename[d]))) {
                    first5_all_digits = false;
                    break;
                }
            }
            if (!first5_all_digits) continue;

            const int seq = std::stoi(filename.substr(0, 5));
            pe_his_by_seq.emplace(seq, entry.path().string());
        }
    } catch (...) {
        // Keep empty index and fall back to exact filename probing.
    }

    const int total = static_cast<int>(frames.size());
    out.clear();
    out.reserve(static_cast<std::size_t>(total));
    bool reported_air_value = false;
    bool reported_dark_offset = false;
    bool reported_raw_only = false;
    bool reported_attenuation_mode = false;
    float raw_air_value = (params.air_value > 1000.0f) ? params.air_value : 60000.0f;
    float raw_dark_offset = 0.0f;
    const bool use_attenuation_image_input = EnvFlag("FDK_INPUT_ATTENUATION_IMAGE", false);
    const bool reverse_projection_image_order = EnvFlag("FDK_REVERSE_PROJECTION_IMAGE_ORDER", false);
    const bool use_mean_detector_centre = EnvFlag("FDK_RTK_USE_MEAN_DETECTOR_CENTRE", false);
    const double mean_detector_residual_shift_scale = EnvDouble("FDK_RTK_MEAN_CENTRE_RESIDUAL_SHIFT_SCALE", 0.0);
    double mean_u_centre_mm = 0.0;
    double mean_v_centre_mm = 0.0;
    if (use_mean_detector_centre && !frames.empty()) {
        for (const FrameInfo& frame : frames) {
            mean_u_centre_mm += static_cast<double>(frame.u_centre);
            mean_v_centre_mm += static_cast<double>(frame.v_centre);
        }
        mean_u_centre_mm /= static_cast<double>(frames.size());
        mean_v_centre_mm /= static_cast<double>(frames.size());
    }

    auto resolve_image_path = [&](int frame_id) -> std::string {
        std::string img_path;
        std::vector<std::string> candidates = {
            params.image_dir + "/frame_" + std::to_string(frame_id) + ".his",
            params.image_dir + "/image_" + std::to_string(frame_id) + ".his",
            params.image_dir + "/" + std::to_string(frame_id) + ".his",
            params.image_dir + "/proj_" + std::to_string(frame_id) + ".his",
            params.image_dir + "/frame_" + std::to_string(frame_id) + ".raw",
        };
        for (const auto& candidate : candidates) {
            if (fs::exists(candidate)) {
                return candidate;
            }
        }

        const int seq_1based = frame_id + 1;
        const auto it = pe_his_by_seq.find(seq_1based);
        if (it != pe_his_by_seq.end()) {
            img_path = it->second;
        }
        return img_path;
    };

    if (progress) {
        progress(0, total,
                 "Using raw projections, frame angles, geometry, and estimated raw-air I0 only.");
        reported_raw_only = true;
    }

    for (int i = 0; i < total; ++i) {
        const FrameInfo& fi = frames[static_cast<std::size_t>(i)];
        const int image_frame_id = reverse_projection_image_order
            ? frames[static_cast<std::size_t>(total - 1 - i)].frame_id
            : fi.frame_id;

        // Build image filename: frame_XXXXXX.his (or .raw / .tif)
        // Try several naming conventions used by XVI and other systems.
        // Also handle Perkin-Elmer DICOM file naming (00001.*.his, 00002.*.his, etc.)
        std::string img_path = resolve_image_path(image_frame_id);

        if (img_path.empty()) {
            if (progress) {
                progress(i, total, "Warning: no image for frame_id=" +
                         std::to_string(image_frame_id) + " – skipped");
            }
            continue;
        }

        std::vector<float> raw_pixels;
        std::string err;
        // Use per-frame detector dimensions if available (from XML), otherwise fall back to params
        const int frame_det_cols = (fi.detector_cols > 0) ? fi.detector_cols : params.detector_cols;
        const int frame_det_rows = (fi.detector_rows > 0) ? fi.detector_rows : params.detector_rows;
        
        if (!ReadHisImage(img_path, frame_det_cols, frame_det_rows,
                          raw_pixels, err)) {
            if (progress) {
                progress(i, total, "Warning: " + err + " – skipped");
            }
            continue;
        }

        const int W = frame_det_cols;
        const int H = frame_det_rows;
        const std::size_t N = static_cast<std::size_t>(W * H);

        ProjectionImage proj;
        proj.width            = W;
        proj.height           = H;
        proj.gantry_angle_deg = fi.gantry_angle_deg;
        proj.frame_id         = fi.frame_id;
        proj.pixels.resize(N);
        // XVI _Frames.xml stores UCentre/VCentre in millimetres on the detector plane.
        // Convert to detector pixels for the rest of this reconstruction path.
        const float detector_size_cm = static_cast<float>(EnvDouble("FDK_DETECTOR_SIZE_CM",
                                        static_cast<double>(params.detector_size_cm)));
        const float detector_pixel_mm = (detector_size_cm * 10.0f) / static_cast<float>(std::max(1, W));
        proj.detector_pixel_size_mm = detector_pixel_mm;
        const double geometry_u_centre_mm = use_mean_detector_centre ? mean_u_centre_mm : static_cast<double>(fi.u_centre);
        const double geometry_v_centre_mm = use_mean_detector_centre ? mean_v_centre_mm : static_cast<double>(fi.v_centre);
        proj.u_centre = static_cast<float>(geometry_u_centre_mm / static_cast<double>(detector_pixel_mm));
        proj.v_centre = static_cast<float>(geometry_v_centre_mm / static_cast<double>(detector_pixel_mm));

        // Raw offset correction + log transform. Pelvis.INI AirValue is a
        // protocol/output reference, while the raw HIS open-beam level is near
        // saturation for this dataset. Estimate the actual raw I0 once from the
        // first valid frame to avoid clipping real patient signal to air.
        if (out.empty()) {
            raw_air_value = EstimateRawAirValue(raw_pixels, raw_air_value);
            const bool use_raw_dark_offset = EnvFlag("FDK_USE_RAW_DARK_OFFSET", false);
            const double auto_dark_offset = (params.air_value > 1000.0f)
                ? std::max(0.0, static_cast<double>(raw_air_value) - static_cast<double>(params.air_value))
                : 0.0;
            raw_dark_offset = use_raw_dark_offset
                ? static_cast<float>(EnvDouble("FDK_RAW_DARK_OFFSET", auto_dark_offset))
                : 0.0f;
            raw_dark_offset = std::max(0.0f, std::min(raw_dark_offset, raw_air_value - 1000.0f));
            if (progress && !reported_air_value) {
                progress(i, total,
                         "[FDK] Raw air I0 estimated as " + std::to_string(raw_air_value) +
                         " ADU (INI AirValue=" + std::to_string(params.air_value) + ")");
                reported_air_value = true;
            }
            if (use_raw_dark_offset && progress && !reported_dark_offset) {
                progress(i, total,
                         "[FDK] Raw dark offset estimated as " + std::to_string(raw_dark_offset) +
                         " ADU; use FDK_USE_RAW_DARK_OFFSET=0 to disable.");
                reported_dark_offset = true;
            }
        }
        const float air_val = raw_air_value;
        const float dark_offset = raw_dark_offset;
        const float corrected_air_val = std::max(1000.0f, air_val - dark_offset);

        if (progress && use_attenuation_image_input && !reported_attenuation_mode) {
            progress(i, total,
                     "Using preprocessed attenuation-image projections (FDK_INPUT_ATTENUATION_IMAGE=1); skipping raw -log(I/I0).");
            reported_attenuation_mode = true;
        }

        if (progress && !reported_raw_only && !use_attenuation_image_input) {
            progress(i, total,
                     "Using raw projections, frame angles, geometry, and estimated raw-air I0 only.");
            reported_raw_only = true;
        }

        auto fill_line_integrals = [&]() {
            float max_line = 0.0f;
            int active_count = 0;
            const float invalid_raw_threshold = air_val * 0.05f;
            const float max_line_integral = static_cast<float>(EnvDouble("FDK_MAX_LINE_INTEGRAL", 4.0));
            if (use_attenuation_image_input) {
                const float low = EstimatePositivePercentile(raw_pixels, 0.01, 0.0f);
                const float high = EstimatePositivePercentile(raw_pixels, 0.995, low + 1.0f);
                const float scale = static_cast<float>(EnvDouble("FDK_ATTENUATION_IMAGE_SCALE", 1.0));
                const float denom = std::max(1.0f, high - low);
                for (std::size_t k = 0; k < N; ++k) {
                    const float normalized = (raw_pixels[k] - low) / denom;
                    const float line = std::max(0.0f, std::min(max_line_integral, normalized * scale));
                    proj.pixels[k] = line;
                    if (line > max_line) max_line = line;
                    if (line > 1.0e-4f) ++active_count;
                }
                return std::make_pair(max_line, static_cast<double>(active_count) / static_cast<double>(std::max<std::size_t>(1, N)));
            }
            for (std::size_t k = 0; k < N; ++k) {
                float pix = raw_pixels[k];

                if (pix < invalid_raw_threshold) {
                    proj.pixels[k] = 0.0f;
                    continue;
                }

                pix -= dark_offset;
                if (pix < 1.0f) pix = 1.0f;
                if (pix > corrected_air_val) pix = corrected_air_val;

                const float line = std::min(max_line_integral, -std::log(pix / corrected_air_val));
                proj.pixels[k] = line;
                if (line > max_line) max_line = line;
                if (line > 1.0e-4f) ++active_count;
            }
            return std::make_pair(max_line, static_cast<double>(active_count) / static_cast<double>(std::max<std::size_t>(1, N)));
        };

        fill_line_integrals();

        if (use_mean_detector_centre && std::fabs(mean_detector_residual_shift_scale) > 1.0e-6) {
            const double residual_u_pixels = (static_cast<double>(fi.u_centre) - mean_u_centre_mm) /
                                             static_cast<double>(detector_pixel_mm);
            const double residual_v_pixels = (static_cast<double>(fi.v_centre) - mean_v_centre_mm) /
                                             static_cast<double>(detector_pixel_mm);
            ShiftProjectionPixels(proj.pixels, W, H,
                                  mean_detector_residual_shift_scale * residual_u_pixels,
                                  mean_detector_residual_shift_scale * residual_v_pixels);
        }

        if (EnvFlag("FDK_RTK_TRANSPOSE_PROJECTIONS", params.rtk_transpose_projections)) {
            TransposeSquareProjectionPixels(proj.pixels, W, H);
        }

        // Zero-out skip regions
        for (int r = 0; r < H; ++r) {
            for (int c = 0; c < params.skip_cols_left && c < W; ++c) {
                proj.pixels[static_cast<std::size_t>(r * W + c)] = 0.0f;
            }
            for (int c = W - params.skip_cols_right; c < W; ++c) {
                if (c >= 0) proj.pixels[static_cast<std::size_t>(r * W + c)] = 0.0f;
            }
        }
        for (int r = 0; r < params.skip_rows_top && r < H; ++r) {
            for (int c = 0; c < W; ++c)
                proj.pixels[static_cast<std::size_t>(r * W + c)] = 0.0f;
        }
        for (int r = H - params.skip_rows_bottom; r < H; ++r) {
            if (r >= 0) for (int c = 0; c < W; ++c)
                proj.pixels[static_cast<std::size_t>(r * W + c)] = 0.0f;
        }

        ApplyProjectionEdgeTaper(proj.pixels, W, H,
                                 static_cast<int>(std::lround(EnvDouble(
                                     "FDK_PROJECTION_EDGE_TAPER_PIXELS",
                                     static_cast<double>(params.projection_edge_taper_pixels)))));

        const std::string custom_half_fan_mode = ToLower(TrimString(std::getenv("FDK_CUSTOM_HALF_FAN_WEIGHT")
            ? std::getenv("FDK_CUSTOM_HALF_FAN_WEIGHT") : params.custom_half_fan_weight));
        ApplyCustomHalfFanWeight(proj.pixels, W, H,
                                 custom_half_fan_mode,
                                 EnvDouble("FDK_CUSTOM_HALF_FAN_WIDTH_PIXELS",
                                           static_cast<double>(params.custom_half_fan_width_pixels)),
                                 EnvDouble("FDK_CUSTOM_HALF_FAN_STRENGTH",
                                           static_cast<double>(params.custom_half_fan_strength)),
                                 static_cast<double>(detector_pixel_mm) / 10.0,
                                 static_cast<double>(detector_size_cm),
                                 static_cast<double>(proj.u_centre) * static_cast<double>(detector_pixel_mm) / 10.0,
                                 static_cast<double>(params.fdd_cm));

        const int virtual_detector_width = static_cast<int>(std::lround(
            EnvDouble("FDK_VIRTUAL_DETECTOR_WIDTH_PIXELS", 0.0)));
        if (virtual_detector_width > W) {
            const double explicit_shift = EnvDouble("FDK_VIRTUAL_DETECTOR_SHIFT_PIXELS",
                                                    std::numeric_limits<double>::quiet_NaN());
            const double shift_pixels = std::isfinite(explicit_shift)
                ? explicit_shift
                : EnvDouble("FDK_VIRTUAL_DETECTOR_SHIFT_SIGN", 1.0) *
                  EnvDouble("FDK_VIRTUAL_DETECTOR_SHIFT_SCALE", static_cast<double>(params.detector_offset_u_scale)) *
                  static_cast<double>(proj.u_centre);
            const double residual_u_centre = EnvDouble("FDK_VIRTUAL_DETECTOR_RESIDUAL_U_SCALE", 0.0) *
                                             static_cast<double>(proj.u_centre);
            ApplyVirtualDetectorRecenter(proj, virtual_detector_width, shift_pixels, residual_u_centre);
        }

        if (EnvFlag("FDK_RTK_FLIP_PROJECTION_U", false)) {
            FlipProjectionPixelsU(proj.pixels, proj.width, proj.height);
        }
        if (EnvFlag("FDK_RTK_FLIP_PROJECTION_V", false)) {
            FlipProjectionPixelsV(proj.pixels, proj.width, proj.height);
        }

        out.push_back(std::move(proj));

        // Log detector centre offsets for first projection (verification)
        if (i == 0 && progress) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "[FDK] Frame 0: u_centre=%.2f px (XML UCentre=%.2f mm), v_centre=%.2f px, proj %dx%d, angle=%.1f deg",
                out.back().u_centre, static_cast<float>(geometry_u_centre_mm), out.back().v_centre,
                out.back().width, out.back().height, fi.gantry_angle_deg);
            progress(0, total, buf);
        }

        if (progress) {
            progress(i + 1, total,
                     "Loaded frame " + std::to_string(fi.frame_id) +
                     " (angle=" + std::to_string(static_cast<int>(fi.gantry_angle_deg)) + " deg)");
        }
    }

    if (out.empty()) {
        error = "No projection images could be loaded from: " + params.image_dir;
        return false;
    }
    return true;
}

// ============================================================================
// RTK FDK 后端
// ============================================================================
FdkEngine::FdkEngine(const FdkParams& params) : m_params(params) {}

namespace {

#ifdef FDK_HAS_RTK
using RtkImageType = itk::Image<float, 3>;

static double DetectorPixelSizeMm(const FdkParams& params, int detector_cols) {
    const double detector_width_mm = EnvDouble("FDK_DETECTOR_SIZE_CM",
                                               static_cast<double>(params.detector_size_cm)) * 10.0;
    return detector_width_mm / static_cast<double>(std::max(1, detector_cols));
}

static double DetectorPixelSizeMm(const FdkParams& params, const ProjectionImage& projection) {
    if (projection.detector_pixel_size_mm > 0.0f) {
        return static_cast<double>(projection.detector_pixel_size_mm);
    }
    return DetectorPixelSizeMm(params, projection.width);
}

static RtkImageType::Pointer CreateProjectionStack(const std::vector<ProjectionImage>& projections,
                                                   const FdkParams& params,
                                                   std::string& error) {
    if (projections.empty() || !projections.front().valid()) {
        error = "No valid projection images.";
        return nullptr;
    }

    const int width = projections.front().width;
    const int height = projections.front().height;
    for (const auto& projection : projections) {
        if (!projection.valid() || projection.width != width || projection.height != height) {
            error = "RTK requires all projections in one reconstruction to have identical dimensions.";
            return nullptr;
        }
    }

    RtkImageType::IndexType start;
    start.Fill(0);
    RtkImageType::SizeType size;
    size[0] = static_cast<RtkImageType::SizeType::SizeValueType>(width);
    size[1] = static_cast<RtkImageType::SizeType::SizeValueType>(height);
    size[2] = static_cast<RtkImageType::SizeType::SizeValueType>(projections.size());
    RtkImageType::RegionType region;
    region.SetIndex(start);
    region.SetSize(size);

    const double pixel_mm = DetectorPixelSizeMm(params, projections.front());
    RtkImageType::SpacingType spacing;
    spacing[0] = pixel_mm;
    spacing[1] = pixel_mm;
    spacing[2] = 1.0;
    RtkImageType::PointType origin;
    const double first_u_mm = static_cast<double>(projections.front().u_centre) * pixel_mm;
    const double first_v_mm = static_cast<double>(projections.front().v_centre) * pixel_mm;
    const double projection_origin_u_mm = EnvDouble("FDK_RTK_PROJECTION_ORIGIN_U_MM", 0.0) +
        EnvDouble("FDK_RTK_PROJECTION_ORIGIN_U_SCALE", 0.0) * first_u_mm;
    const double projection_origin_v_mm = EnvDouble("FDK_RTK_PROJECTION_ORIGIN_V_MM", 0.0) +
        EnvDouble("FDK_RTK_PROJECTION_ORIGIN_V_SCALE", 0.0) * first_v_mm;
    origin[0] = -0.5 * static_cast<double>(width - 1) * pixel_mm + projection_origin_u_mm;
    origin[1] = -0.5 * static_cast<double>(height - 1) * pixel_mm + projection_origin_v_mm;
    origin[2] = 0.0;

    auto image = RtkImageType::New();
    image->SetRegions(region);
    image->SetSpacing(spacing);
    image->SetOrigin(origin);
    image->Allocate();

    float* buffer = image->GetBufferPointer();
    const std::size_t plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t z = 0; z < projections.size(); ++z) {
        std::copy(projections[z].pixels.begin(), projections[z].pixels.end(), buffer + z * plane);
    }
    return image;
}

static rtk::ThreeDCircularProjectionGeometry::Pointer CreateRtkGeometry(
    const std::vector<ProjectionImage>& projections,
    const FdkParams& params,
    std::string& geometry_message) {
    const bool use_elekta_xml_geometry = EnvFlag("FDK_USE_RTK_ELEKTA_XML_GEOMETRY", false);
    if (use_elekta_xml_geometry && !params.frame_info_file.empty()) {
        std::string lower_path = ToLower(params.frame_info_file);
        if (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".xml") {
            try {
                auto reader = rtk::ElektaXVI5GeometryXMLFileReader::New();
                reader->SetFilename(params.frame_info_file.c_str());
                reader->GenerateOutputInformation();
                auto source_geometry = reader->GetGeometry();
                const auto& sid = source_geometry->GetSourceToIsocenterDistances();
                const auto& sdd = source_geometry->GetSourceToDetectorDistances();
                const auto& gantry = source_geometry->GetGantryAngles();
                const auto& projection_offset_x = source_geometry->GetProjectionOffsetsX();
                const auto& projection_offset_y = source_geometry->GetProjectionOffsetsY();
                const auto& out_of_plane = source_geometry->GetOutOfPlaneAngles();
                const auto& in_plane = source_geometry->GetInPlaneAngles();
                const auto& source_offset_x = source_geometry->GetSourceOffsetsX();
                const auto& source_offset_y = source_geometry->GetSourceOffsetsY();
                auto geometry = rtk::ThreeDCircularProjectionGeometry::New();
                for (const auto& projection : projections) {
                    const std::size_t index = static_cast<std::size_t>(std::max(0, projection.frame_id));
                    if (index >= gantry.size()) {
                        geometry = nullptr;
                        break;
                    }
                    geometry->AddProjectionInRadians(sid[index], sdd[index], gantry[index],
                                                     projection_offset_x[index], projection_offset_y[index],
                                                     out_of_plane[index], in_plane[index],
                                                     source_offset_x[index], source_offset_y[index]);
                }
                if (geometry && geometry->GetGantryAngles().size() == projections.size()) {
                    geometry_message = "RTK: using Elekta XVI XML geometry reader (" +
                                       std::to_string(projections.size()) + " projections)";
                    return geometry;
                }
                geometry_message = "RTK: Elekta XML geometry frame count mismatch; using manual geometry fallback";
            } catch (const std::exception& ex) {
                geometry_message = std::string("RTK: Elekta XML geometry reader failed; using manual geometry fallback: ") + ex.what();
            } catch (...) {
                geometry_message = "RTK: Elekta XML geometry reader failed; using manual geometry fallback";
            }
        }
    }

    auto geometry = rtk::ThreeDCircularProjectionGeometry::New();
    const double sid_mm = static_cast<double>(params.fid_cm) * 10.0;
    const double sdd_mm = static_cast<double>(params.fdd_cm) * 10.0;

    const double u_sign = EnvDouble("FDK_RTK_U_OFFSET_SIGN", 1.0);
    const double u_scale = EnvDouble("FDK_RTK_U_OFFSET_SCALE", static_cast<double>(params.detector_offset_u_scale));
    const double v_sign = EnvDouble("FDK_RTK_V_OFFSET_SIGN", -1.0);
    const double v_scale = EnvDouble("FDK_RTK_V_OFFSET_SCALE", static_cast<double>(params.detector_offset_v_scale));
    const double angle_sign = EnvDouble("FDK_RTK_ANGLE_SIGN", 1.0);
    const double angle_offset_deg = EnvDouble("FDK_RTK_ANGLE_OFFSET_DEG", 0.0);
    const double out_of_plane_deg = EnvDouble("FDK_RTK_OUT_OF_PLANE_ANGLE_DEG", 0.0);
    const double in_plane_deg = EnvDouble("FDK_RTK_IN_PLANE_ANGLE_DEG", 0.0);
    const double source_u_scale = EnvDouble("FDK_RTK_SOURCE_U_OFFSET_SCALE", 0.0);
    const double source_v_scale = EnvDouble("FDK_RTK_SOURCE_V_OFFSET_SCALE", 0.0);
    if (geometry_message.empty()) {
        geometry_message = "RTK: using manual projection geometry fallback";
    }

    const bool use_relative_centre = EnvFlag("FDK_RTK_USE_RELATIVE_CENTRE", false);
    const double reference_u_centre = (!projections.empty()) ? static_cast<double>(projections.front().u_centre) : 0.0;
    const double reference_v_centre = (!projections.empty()) ? static_cast<double>(projections.front().v_centre) : 0.0;

    struct Vec3 {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };
    const auto rotate_vec = [](Vec3 v, char axis, double angle_rad) -> Vec3 {
        const double cs = std::cos(angle_rad);
        const double sn = std::sin(angle_rad);
        if (axis == 'x') return {v.x, v.y * cs - v.z * sn, v.y * sn + v.z * cs};
        if (axis == 'z') return {v.x * cs - v.y * sn, v.x * sn + v.y * cs, v.z};
        return {v.x * cs + v.z * sn, v.y, -v.x * sn + v.z * cs};
    };
    const auto scale_vec = [](Vec3 v, double scale) -> Vec3 {
        return {v.x * scale, v.y * scale, v.z * scale};
    };
    const auto add_vec = [](Vec3 a, Vec3 b) -> Vec3 {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    };
    const auto cross_vec = [](Vec3 a, Vec3 b) -> Vec3 {
        return {a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
    };
    const auto parse_axis_vec = [](const char* text, Vec3 fallback) -> Vec3 {
        if (!text || !*text) return fallback;
        std::string value = ToLower(TrimString(text));
        double sign = 1.0;
        if (!value.empty() && (value[0] == '+' || value[0] == '-')) {
            sign = (value[0] == '-') ? -1.0 : 1.0;
            value.erase(value.begin());
        }
        if (value == "x" || value == "0") return {sign, 0.0, 0.0};
        if (value == "y" || value == "1") return {0.0, sign, 0.0};
        if (value == "z" || value == "2") return {0.0, 0.0, sign};
        return fallback;
    };

    const std::string explicit_axis_text = ToLower(TrimString(std::getenv("FDK_RTK_EXPLICIT_GEOMETRY_AXIS")
        ? std::getenv("FDK_RTK_EXPLICIT_GEOMETRY_AXIS") : params.rtk_explicit_geometry_axis));
    const bool use_explicit_geometry = explicit_axis_text == "x" || explicit_axis_text == "y" || explicit_axis_text == "z";
    if (use_explicit_geometry) {
        auto explicit_geometry = rtk::ThreeDCircularProjectionGeometry::New();
        const char axis = explicit_axis_text[0];
        const char* row_axis_env = std::getenv("FDK_RTK_EXPLICIT_ROW_AXIS");
        const char* col_axis_env = std::getenv("FDK_RTK_EXPLICIT_COL_AXIS");
        const Vec3 base_row = parse_axis_vec(row_axis_env ? row_axis_env : params.rtk_explicit_row_axis.c_str(), {1.0, 0.0, 0.0});
        const Vec3 base_col = parse_axis_vec(col_axis_env ? col_axis_env : params.rtk_explicit_col_axis.c_str(), {0.0, 1.0, 0.0});
        const Vec3 base_normal = cross_vec(base_row, base_col);
        for (const auto& projection : projections) {
            const double pixel_mm = DetectorPixelSizeMm(params, projection);
            const double u_centre = static_cast<double>(projection.u_centre) - (use_relative_centre ? reference_u_centre : 0.0);
            const double v_centre = static_cast<double>(projection.v_centre) - (use_relative_centre ? reference_v_centre : 0.0);
            const double offset_x_mm = u_sign * u_scale * u_centre * pixel_mm;
            const double offset_y_mm = v_sign * v_scale * v_centre * pixel_mm;
            const double source_offset_x_mm = u_sign * source_u_scale * u_centre * pixel_mm;
            const double source_offset_y_mm = v_sign * source_v_scale * v_centre * pixel_mm;
            const double angle_rad = (angle_sign * static_cast<double>(projection.gantry_angle_deg) + angle_offset_deg) * M_PI / 180.0;

            const Vec3 row = rotate_vec(base_row, axis, angle_rad);
            const Vec3 col = rotate_vec(base_col, axis, angle_rad);
            const Vec3 normal = rotate_vec(base_normal, axis, angle_rad);
                const Vec3 source = add_vec(add_vec(scale_vec(normal, sid_mm),
                                scale_vec(row, source_offset_x_mm)),
                            scale_vec(col, source_offset_y_mm));
            const Vec3 detector = add_vec(add_vec(scale_vec(normal, sid_mm - sdd_mm),
                                                  scale_vec(row, offset_x_mm)),
                                          scale_vec(col, offset_y_mm));

            rtk::ThreeDCircularProjectionGeometry::PointType source_point;
            rtk::ThreeDCircularProjectionGeometry::PointType detector_point;
            rtk::ThreeDCircularProjectionGeometry::VectorType row_vector;
            rtk::ThreeDCircularProjectionGeometry::VectorType col_vector;
            source_point[0] = source.x; source_point[1] = source.y; source_point[2] = source.z;
            detector_point[0] = detector.x; detector_point[1] = detector.y; detector_point[2] = detector.z;
            row_vector[0] = row.x; row_vector[1] = row.y; row_vector[2] = row.z;
            col_vector[0] = col.x; col_vector[1] = col.y; col_vector[2] = col.z;
            if (!explicit_geometry->AddProjection(source_point, detector_point, row_vector, col_vector)) {
                geometry_message = "RTK: explicit source/detector geometry failed; using manual projection geometry fallback";
                explicit_geometry = nullptr;
                break;
            }
        }
        if (explicit_geometry && explicit_geometry->GetGantryAngles().size() == projections.size()) {
            geometry_message = "RTK: using explicit source/detector geometry axis=" + explicit_axis_text;
            return explicit_geometry;
        }
    }

    for (const auto& projection : projections) {
        const double pixel_mm = DetectorPixelSizeMm(params, projection);
        const double u_centre = static_cast<double>(projection.u_centre) - (use_relative_centre ? reference_u_centre : 0.0);
        const double v_centre = static_cast<double>(projection.v_centre) - (use_relative_centre ? reference_v_centre : 0.0);
        const double offset_x_mm = u_sign * u_scale * u_centre * pixel_mm;
        const double offset_y_mm = v_sign * v_scale * v_centre * pixel_mm;
        const double source_offset_x_mm = u_sign * source_u_scale * u_centre * pixel_mm;
        const double source_offset_y_mm = v_sign * source_v_scale * v_centre * pixel_mm;
        const double angle_deg = angle_sign * static_cast<double>(projection.gantry_angle_deg) + angle_offset_deg;
        geometry->AddProjection(sid_mm, sdd_mm,
                                angle_deg,
                                offset_x_mm, offset_y_mm,
                    out_of_plane_deg, in_plane_deg,
                    source_offset_x_mm, source_offset_y_mm);
    }
    return geometry;
}

static RtkImageType::Pointer CreateEmptyVolumeSource(const FdkParams& params) {
    const int volume_nx = std::max(1, EnvInt("FDK_VOLUME_DIM_X", params.nx));
    const int volume_ny = std::max(1, EnvInt("FDK_VOLUME_DIM_Y", params.ny));
    const int volume_nz = std::max(1, EnvInt("FDK_VOLUME_DIM_Z", params.nz));

    RtkImageType::IndexType start;
    start.Fill(0);
    RtkImageType::SizeType size;
    size[0] = static_cast<RtkImageType::SizeType::SizeValueType>(volume_nx);
    size[1] = static_cast<RtkImageType::SizeType::SizeValueType>(volume_ny);
    size[2] = static_cast<RtkImageType::SizeType::SizeValueType>(volume_nz);
    RtkImageType::RegionType region;
    region.SetIndex(start);
    region.SetSize(size);

    const double spacing_mm = static_cast<double>(params.voxel_size_cm) * 10.0;
    RtkImageType::SpacingType spacing;
    spacing[0] = spacing_mm;
    spacing[1] = spacing_mm;
    spacing[2] = spacing_mm;

    int direction_axes[3] = {0, 1, 2};
    double direction_signs[3] = {1.0, 1.0, 1.0};
    ParseVolumeDirectionAxes(std::getenv("FDK_VOLUME_DIRECTION_AXES"), direction_axes);
    ParseVolumeDirectionSigns(std::getenv("FDK_VOLUME_DIRECTION_SIGNS"), direction_signs);

    RtkImageType::DirectionType direction;
    direction.Fill(0.0);
    for (int index_axis = 0; index_axis < 3; ++index_axis) {
        direction[direction_axes[index_axis]][index_axis] = direction_signs[index_axis];
    }

    RtkImageType::PointType origin;
    const double offset_x_cm = EnvDouble("FDK_VOLUME_OFFSET_X_CM", static_cast<double>(params.offset_x_cm));
    const double offset_y_cm = EnvDouble("FDK_VOLUME_OFFSET_Y_CM", static_cast<double>(params.offset_y_cm));
    const double offset_z_cm = EnvDouble("FDK_VOLUME_OFFSET_Z_CM", static_cast<double>(params.offset_z_cm));
    const bool offset_in_index_axes = EnvFlag("FDK_VOLUME_OFFSET_IN_INDEX_AXES",
                                              std::getenv("FDK_VOLUME_DIRECTION_AXES") != nullptr);
    const double offset_index_mm[3] = {offset_x_cm * 10.0, offset_y_cm * 10.0, offset_z_cm * 10.0};
    const double half_index_mm[3] = {
        0.5 * static_cast<double>(volume_nx - 1) * spacing_mm,
        0.5 * static_cast<double>(volume_ny - 1) * spacing_mm,
        0.5 * static_cast<double>(volume_nz - 1) * spacing_mm};
    double center_phys_mm[3] = {offset_x_cm * 10.0, offset_y_cm * 10.0, offset_z_cm * 10.0};
    if (offset_in_index_axes) {
        center_phys_mm[0] = center_phys_mm[1] = center_phys_mm[2] = 0.0;
        for (int index_axis = 0; index_axis < 3; ++index_axis) {
            const int phys_axis = direction_axes[index_axis];
            center_phys_mm[phys_axis] += direction_signs[index_axis] * offset_index_mm[index_axis];
        }
    }
    origin[0] = center_phys_mm[0];
    origin[1] = center_phys_mm[1];
    origin[2] = center_phys_mm[2];
    for (int index_axis = 0; index_axis < 3; ++index_axis) {
        const int phys_axis = direction_axes[index_axis];
        origin[phys_axis] -= direction_signs[index_axis] * half_index_mm[index_axis];
    }

    auto image = RtkImageType::New();
    image->SetRegions(region);
    image->SetSpacing(spacing);
    image->SetOrigin(origin);
    image->SetDirection(direction);
    image->Allocate();
    image->FillBuffer(0.0f);
    return image;
}

static void ConfigureRampFilter(rtk::FDKConeBeamReconstructionFilter<RtkImageType>::Pointer fdk,
                                const FdkParams& params) {
    const std::string filter = ToLower(params.filter);
    fdk->GetRampFilter()->SetTruncationCorrection(0.0);
    const double cutoff = (params.filter_param_a > 0.0f && params.filter_param_a <= 1.0f)
        ? static_cast<double>(params.filter_param_a)
        : 1.0;
    if (filter == "hamming") {
        fdk->GetRampFilter()->SetHammingFrequency(cutoff);
    } else if (filter == "hanning" || filter == "hann") {
        fdk->GetRampFilter()->SetHannCutFrequency(cutoff);
    } else if (filter == "ramlak" || filter == "ram-lak") {
        fdk->GetRampFilter()->SetRamLakCutFrequency(cutoff);
    }
}

static bool ReconstructWithRtk(const std::vector<ProjectionImage>& projections,
                               const FdkParams& params,
                               Volume3D& volume_out,
                               std::function<void(const ReconProgress&)> progress,
                               std::string& error) {
    auto report = [&progress](float pct, const std::string& message) {
        if (!progress) {
            return;
        }
        ReconProgress rp;
        rp.pct = pct;
        rp.message = message;
        progress(rp);
    };

    report(2.0f, "RTK: creating projection stack...");
    auto projection_stack = CreateProjectionStack(projections, params, error);
    if (!projection_stack) return false;

    report(5.0f, "RTK: creating projection geometry...");
    std::string geometry_message;
    auto geometry = CreateRtkGeometry(projections, params, geometry_message);
    if (!geometry_message.empty()) {
        report(6.0f, geometry_message);
    }
    auto volume_source = CreateEmptyVolumeSource(params);

    const bool use_displaced_detector = EnvFlag("FDK_USE_RTK_DISPLACED_DETECTOR", false);
    const std::string displaced_mode = ToLower(TrimString(std::getenv("FDK_RTK_DISPLACED_MODE")
        ? std::getenv("FDK_RTK_DISPLACED_MODE") : "offfov"));
    report(8.0f, use_displaced_detector
                    ? ("RTK: displaced-detector filter enabled (mode=" + displaced_mode + ")")
                    : "RTK: displaced-detector filter disabled (set FDK_USE_RTK_DISPLACED_DETECTOR=1 to enable)");

    using OffsetFovDisplacedDetectorType = rtk::DisplacedDetectorForOffsetFieldOfViewImageFilter<RtkImageType>;
    using WangDisplacedDetectorType = rtk::DisplacedDetectorImageFilter<RtkImageType>;
    OffsetFovDisplacedDetectorType::Pointer offset_fov_displaced_detector;
    WangDisplacedDetectorType::Pointer wang_displaced_detector;
    RtkImageType::Pointer weighted_projection_input = projection_stack;
    if (use_displaced_detector) {
        if (displaced_mode == "wang" || displaced_mode == "classic" || displaced_mode == "wang_nopad") {
            wang_displaced_detector = WangDisplacedDetectorType::New();
            wang_displaced_detector->SetInput(projection_stack);
            wang_displaced_detector->SetGeometry(geometry);
            wang_displaced_detector->SetDisable(false);
            wang_displaced_detector->SetPadOnTruncatedSide(displaced_mode != "wang_nopad");
            weighted_projection_input = wang_displaced_detector->GetOutput();
        } else {
            offset_fov_displaced_detector = OffsetFovDisplacedDetectorType::New();
            offset_fov_displaced_detector->SetInput(projection_stack);
            offset_fov_displaced_detector->SetGeometry(geometry);
            offset_fov_displaced_detector->SetDisable(false);
            weighted_projection_input = offset_fov_displaced_detector->GetOutput();
        }
    }

    using ParkerType = rtk::ParkerShortScanImageFilter<RtkImageType>;
    ParkerType::Pointer parker;
    if (params.short_scan) {
        parker = ParkerType::New();
        parker->SetInput(weighted_projection_input);
        parker->SetGeometry(geometry);
        parker->SetAngularGapThreshold(static_cast<double>(params.cone_angle_deg) * M_PI / 180.0);
    }

    using FdkFilterType = rtk::FDKConeBeamReconstructionFilter<RtkImageType>;
    auto fdk_filter = FdkFilterType::New();
    fdk_filter->SetInput(0, volume_source);
    RtkImageType* reconstruction_input = weighted_projection_input.GetPointer();
    if (params.short_scan && parker) {
        reconstruction_input = parker->GetOutput();
    }
    fdk_filter->SetInput(1, reconstruction_input);
    fdk_filter->SetGeometry(geometry);
    fdk_filter->SetProjectionSubsetSize(16);
    ConfigureRampFilter(fdk_filter, params);

    using FovFilterType = rtk::FieldOfViewImageFilter<RtkImageType, RtkImageType>;
    FovFilterType::Pointer field_of_view;
    const bool use_fov_filter = EnvFlag("FDK_USE_RTK_FOV_FILTER", false);
    if (use_fov_filter) {
        field_of_view = FovFilterType::New();
        field_of_view->SetInput(fdk_filter->GetOutput());
        field_of_view->SetProjectionsStack(projection_stack);
        field_of_view->SetGeometry(geometry);
    }

    auto progress_command = itk::FunctionCommand::New();
    int last_reported_update_pct = 10;
    progress_command->SetCallback([&](const itk::EventObject&) {
        const double filter_progress = std::max(0.0, std::min(1.0, static_cast<double>(fdk_filter->GetProgress())));
        const int update_pct = static_cast<int>(10.0 + 84.0 * filter_progress);
        if (update_pct >= last_reported_update_pct + 2 && update_pct < 95) {
            last_reported_update_pct = update_pct;
            report(static_cast<float>(update_pct),
                   "RTK: FDK filter update progress " + std::to_string(update_pct) + "%");
        }
    });
    fdk_filter->AddObserver(itk::ProgressEvent(), progress_command);

    try {
        report(10.0f, "RTK: running FDK filter update...");
        if (field_of_view) {
            field_of_view->Update();
        } else {
            fdk_filter->Update();
        }
    } catch (const itk::ExceptionObject& ex) {
        error = std::string("RTK FDK reconstruction failed: ") + ex.GetDescription();
        return false;
    } catch (const std::exception& ex) {
        error = std::string("RTK FDK reconstruction failed: ") + ex.what();
        return false;
    }

    report(95.0f, "RTK: copying output volume...");
    auto output = field_of_view ? field_of_view->GetOutput() : fdk_filter->GetOutput();
    const auto output_region = output->GetBufferedRegion();
    const auto output_size = output_region.GetSize();
    const std::size_t output_voxel_count =
        static_cast<std::size_t>(output_size[0]) *
        static_cast<std::size_t>(output_size[1]) *
        static_cast<std::size_t>(output_size[2]);
    report(96.0f, "RTK: output buffered region " +
                  std::to_string(static_cast<unsigned long long>(output_size[0])) + "x" +
                  std::to_string(static_cast<unsigned long long>(output_size[1])) + "x" +
                  std::to_string(static_cast<unsigned long long>(output_size[2])));
    if (output_size[0] <= 0 || output_size[1] <= 0 || output_size[2] <= 0 ||
        output->GetBufferPointer() == nullptr || output_voxel_count == 0) {
        error = "RTK FDK reconstruction produced an empty output volume.";
        return false;
    }

    volume_out.nx = static_cast<int>(output_size[0]);
    volume_out.ny = static_cast<int>(output_size[1]);
    volume_out.nz = static_cast<int>(output_size[2]);
    volume_out.voxel_size_cm = params.voxel_size_cm;
    volume_out.data.resize(output_voxel_count);
    const float* output_buffer = output->GetBufferPointer();
    std::copy(output_buffer, output_buffer + output_voxel_count, volume_out.data.begin());
    if (ApplyOutputReorientationIfRequested(volume_out)) {
        report(98.5f, "RTK: output volume reoriented by FDK_OUTPUT_REORIENT_AXES");
    }
    report(98.0f, "RTK: output volume copied to application buffer");
    return true;
}
#endif

static std::string FormatVolumeStats(const char* label, const Volume3D& vol) {
    if (!vol.valid() || vol.data.empty()) {
        return std::string(label) + ": empty volume";
    }

    float vmin = vol.data.front();
    float vmax = vol.data.front();
    double sum = 0.0;
    for (float v : vol.data) {
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        sum += v;
    }

    std::ostringstream oss;
    oss << label << ": min=" << vmin
        << " max=" << vmax
        << " mean=" << (sum / static_cast<double>(vol.data.size()))
        << " count=" << vol.data.size();
    return oss.str();
}

static void ReportProgressMessage(std::function<void(const ReconProgress&)>& progress,
                                  float pct,
                                  const std::string& message) {
    if (!progress) {
        return;
    }
    ReconProgress rp;
    rp.pct = pct;
    rp.message = message;
    progress(rp);
}

} // namespace

bool FdkEngine::Reconstruct(const std::vector<ProjectionImage>& projections,
                             Volume3D& volume_out,
                             std::function<void(const ReconProgress&)> progress) {
    if (projections.empty()) {
        ReconProgress rp;
        rp.error = true;
        rp.message = "No projection images";
        if (progress) progress(rp);
        return false;
    }

    if (progress) {
        ReconProgress rp;
        rp.frames_done = 0;
        rp.frames_total = static_cast<int>(projections.size());
        rp.pct = 0.0f;
        rp.message = "Running RTK FDK reconstruction...";
        progress(rp);
    }

#ifndef FDK_HAS_RTK
    ReconProgress missing;
    missing.error = true;
    missing.message = "RTK backend is not available in this build.";
    if (progress) progress(missing);
    return false;
#else
    std::string error;
    if (!ReconstructWithRtk(projections, m_params, volume_out, progress, error)) {
        ReconProgress rp;
        rp.error = true;
        rp.message = error;
        if (progress) progress(rp);
        return false;
    }

    ReportProgressMessage(progress, 98.5f, FormatVolumeStats("Pre-PostProcess stats", volume_out));
    ReportProgressMessage(progress, 99.0f,
                          "Applying output scale=" + std::to_string(m_params.scale_out) +
                          " offset=" + std::to_string(m_params.offset_out));
    PostProcess(volume_out);
    ReportProgressMessage(progress, 99.5f, FormatVolumeStats("Post-PostProcess stats", volume_out));

    if (progress) {
        ReconProgress rp;
        rp.frames_done = static_cast<int>(projections.size());
        rp.frames_total = static_cast<int>(projections.size());
        rp.pct = 100.0f;
        rp.finished = true;
        rp.message = "RTK FDK reconstruction complete (" + std::to_string(projections.size()) + " projections)";
        progress(rp);
    }
    return true;
#endif
}

void FdkEngine::PostProcess(Volume3D& vol, bool clamp_non_negative) const {
    // RTK 已完成滤波、短扫权重和反投影；这里只做应用层输出映射。
    // HU = voxel × scale_out + offset_out
    // 典型配置：scale_out=1000, offset_out=-1000 → 空气 ≈ -1000 HU
    const float scale  = m_params.scale_out;
    const float offset = m_params.offset_out;
    const float non_neg= m_params.non_negativity_threshold;

    // ┌─ 诊断：计算min/max/mean ──────────────────────────────────────────┐
    float val_min = std::numeric_limits<float>::max();
    float val_max = std::numeric_limits<float>::lowest();
    float val_sum = 0.0f;
    std::size_t val_count = vol.data.size();
    
    for (const float& v : vol.data) {
        if (v < val_min) val_min = v;
        if (v > val_max) val_max = v;
        val_sum += v;
    }
    float val_mean = (val_count > 0) ? val_sum / static_cast<float>(val_count) : 0.0f;
    std::cout << "[FDK] Pre-PostProcess Stats: min=" << val_min << " max=" << val_max 
              << " mean=" << val_mean << " count=" << val_count << std::endl;
    // └──────────────────────────────────────────────────────────────────┘

    for (float& v : vol.data) {
        v = v * scale + offset;
        if (clamp_non_negative && v < non_neg) v = non_neg;
    }
    
    // ┌─ 诊断：计算Post-PostProcess min/max ──────────────────────────────┐
    val_min = std::numeric_limits<float>::max();
    val_max = std::numeric_limits<float>::lowest();
    val_sum = 0.0f;
    for (const float& v : vol.data) {
        if (v < val_min) val_min = v;
        if (v > val_max) val_max = v;
        val_sum += v;
    }
    val_mean = (val_count > 0) ? val_sum / static_cast<float>(val_count) : 0.0f;
    std::cout << "[FDK] Post-PostProcess Stats: min=" << val_min << " max=" << val_max 
              << " mean=" << val_mean << " (after scale=" << scale << " offset=" << offset << ")" << std::endl;
    // └──────────────────────────────────────────────────────────────────┘
}

} // namespace fdk
