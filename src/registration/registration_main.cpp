#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <limits>
#include <vector>

#include "registration/registration_ui.h"
#include "registration/registration.h"
#include "registration/transform.h"

#include <vtkDICOMImageReader.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkSmartPointer.h>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>
#include <wx/wx.h>
#include <wx/thread.h>

namespace {

// -----------------------------------------------------------------------------
// 配准主流程说明（GUI 层）
//
// 本文件承担三类职责：
// 1) 从 GUI/参数层选择配准算法（Seed / Bone / Grey value）；
// 2) 组织分阶段优化（粗到细），并控制每阶段的超时、无改进提前停止、采样步长；
// 3) 将 Registration 内核结果回传到界面日志和最终结果结构。
//
// 注意：这里不直接实现代价函数，而是通过 registration::Registration 注入
// metric / transform_type / simplex 参数来驱动底层求解。
// -----------------------------------------------------------------------------

// 自定义事件用于线程通信
wxDECLARE_EVENT(wxEVT_REG_LOG, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_REG_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_REG_ERROR, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_VOLUMES_LOADED, wxCommandEvent);

wxDEFINE_EVENT(wxEVT_REG_LOG, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_REG_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_REG_ERROR, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_VOLUMES_LOADED, wxCommandEvent);

bool IsIniPath(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    for (char& ch : ext) {
        ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
    }
    return ext == ".INI";
}

int ParseState(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (...) {
        return -1;
    }
}

struct StartupInfo {
    std::string mode = "Default";
    int state = 3;
    std::string ini_path;
    bool fastmode = false;
    std::vector<std::string> lines;
};

enum class MatchAlgorithm {
    kSeed,
    kBone,
    kGreyValue
};

wxString MatchAlgorithmToLabel(MatchAlgorithm algorithm) {
    switch (algorithm) {
        case MatchAlgorithm::kSeed:
            return wxT("Seed");
        case MatchAlgorithm::kBone:
            return wxT("Bone");
        case MatchAlgorithm::kGreyValue:
            return wxT("Grey value");
    }
    return wxT("Grey value");
}

MatchAlgorithm MatchAlgorithmFromLabel(const wxString& label) {
    if (label.CmpNoCase(wxT("Seed")) == 0) {
        return MatchAlgorithm::kSeed;
    }
    if (label.CmpNoCase(wxT("Bone")) == 0) {
        return MatchAlgorithm::kBone;
    }
    return MatchAlgorithm::kGreyValue;
}

struct MatchAlgorithmConfig {
    registration::Metric metric;
    double tolerance;
    double offset;
};

MatchAlgorithmConfig GetMatchAlgorithmConfig(MatchAlgorithm algorithm) {
    // 每种算法使用不同的代价函数和 simplex 初始参数。
    // tolerance 越小，收敛判定越严格；offset 越大，初始搜索步长越激进。
    switch (algorithm) {
        case MatchAlgorithm::kSeed:
            return {registration::Metric::kNormMutualInfo, 0.001, 8.0};
        case MatchAlgorithm::kBone:
            return {registration::Metric::kRmsAdjust, 0.005, 10.0};
        case MatchAlgorithm::kGreyValue:
            // Keep Grey value settings close to external REGISTRATION defaults.
            return {registration::Metric::kCorrRatio, 0.0001, 3.0};
    }
    return {registration::Metric::kCorrRatio, 0.0001, 3.0};
}

StartupInfo ParseStartupInfo(int argc, char** argv) {
    StartupInfo info;
    info.lines.push_back("argc=" + std::to_string(argc));
    for (int i = 0; i < argc; ++i) {
        info.lines.push_back("argv[" + std::to_string(i) + "]=" + std::string(argv[i]));
    }

    if (argc == 3) {
        const std::string arg1 = argv[1];
        const std::string arg2 = argv[2];
        const int state = ParseState(arg1);

        if (state >= 1 && state <= 6 && IsIniPath(arg2)) {
            info.mode = "Elekta";
            info.state = state;
            info.ini_path = arg2;
            info.lines.push_back("State=" + std::to_string(state));
            info.lines.push_back("IniPath=" + arg2);
        } else {
            info.mode = "Standalone";
            info.lines.push_back("Arg1=" + arg1);
            info.lines.push_back("Arg2=" + arg2);
        }
    } else if (argc == 2) {
        const std::string arg1 = argv[1];
        if (arg1 == "/fastmode" || arg1 == "/FASTMODE") {
            info.mode = "StandaloneFastMode";
            info.fastmode = true;
            info.lines.push_back("Fast mode requested");
        } else if (IsIniPath(arg1)) {
            info.mode = "IniOnly";
            info.ini_path = arg1;
            info.lines.push_back("IniPath=" + arg1);
        } else {
            info.mode = "StandaloneSelectLatestScan";
            info.lines.push_back("ScanId=" + arg1);
        }
    }

    return info;
}

bool LooksLikeDicomFile(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }
    std::string ext = path.extension().string();
    for (char& ch : ext) {
        ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
    }
    if (ext == ".DCM") {
        return true;
    }
    return ext.empty();
}

size_t CountDicomFilesInDir(const wxString& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir_path(std::string(directory.mb_str()));
    if (!fs::is_directory(dir_path, ec)) {
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        if (ec) {
            break;
        }
        if (LooksLikeDicomFile(entry.path())) {
            ++count;
        }
    }
    return count;
}

wxString DefaultDicomDataRoot() {
    return wxT("C:\\code test\\dicom data");
}

wxString FirstExistingDirectory(const std::vector<wxString>& candidates) {
    for (const wxString& candidate : candidates) {
        if (!candidate.IsEmpty() && wxDirExists(candidate)) {
            return candidate;
        }
    }
    return candidates.empty() ? wxString() : candidates.front();
}

wxString DefaultRegistrationIniPath() {
    const wxString default_ini = DefaultDicomDataRoot() + wxT("\\demo_case_registration.ini");
    if (wxFileExists(default_ini)) {
        return default_ini;
    }
    return default_ini;
}

wxString DefaultReferenceDicomDir() {
    const wxString root = DefaultDicomDataRoot();
    return FirstExistingDirectory({
        root + wxT("\\3D Volumn\\1501"),
        root + wxT("\\3D Volumn\\Bladder Pre Iris"),
        root + wxT("\\3D Volumn")
    });
}

wxString DefaultOnlineDicomDir() {
    const wxString root = DefaultDicomDataRoot();
    return FirstExistingDirectory({
        root + wxT("\\3D Volumn\\1519"),
        root + wxT("\\3D Volumn\\1501"),
        root + wxT("\\3D Volumn\\Bladder Pre Iris"),
        root + wxT("\\3D Volumn")
    });
}

wxString ExistingDirectoryOrDicomDataRoot(const wxString& preferred) {
    if (!preferred.IsEmpty() && wxDirExists(preferred)) {
        return preferred;
    }
    const wxString root = DefaultDicomDataRoot();
    if (wxDirExists(root)) {
        return root;
    }
    return preferred;
}

bool LoadDicomVolumeAsImage3D(
    const wxString& directory,
    registration::Image3D& out,
    wxString& error,
    std::array<double, 3>* out_spacing = nullptr,
    std::array<double, 3>* out_origin = nullptr) {
    // Convert wxString to std::string with safe lifetime management
    std::string dir_path = std::string(directory.mb_str());
    // Normalize path separators for VTK (use forward slashes)
    for (auto& ch : dir_path) {
        if (ch == '\\') ch = '/';
    }

    vtkNew<vtkDICOMImageReader> reader;
    try {
        reader->SetDirectoryName(dir_path.c_str());
        reader->Update();
    } catch (const std::exception& ex) {
        error = wxString::Format(wxT("VTK DICOM reader error: %s"), wxString::FromUTF8(ex.what()));
        return false;
    } catch (...) {
        error = wxT("VTK DICOM reader encountered an unknown error.");
        return false;
    }

    vtkImageData* image = reader->GetOutput();
    if (!image) {
        error = wxT("vtkDICOMImageReader returned null image.");
        return false;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        error = wxString::Format(wxT("Invalid DICOM dimensions: %d x %d x %d"), dims[0], dims[1], dims[2]);
        return false;
    }

    if (out_spacing) {
        double spacing[3] = {1.0, 1.0, 1.0};
        image->GetSpacing(spacing);
        *out_spacing = {spacing[0], spacing[1], spacing[2]};
    }
    if (out_origin) {
        double origin[3] = {0.0, 0.0, 0.0};
        image->GetOrigin(origin);
        *out_origin = {origin[0], origin[1], origin[2]};
    }

    out.size = {dims[0], dims[1], dims[2]};
    out.voxels.resize(out.voxel_count());

    std::size_t idx = 0;
    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                const double v = image->GetScalarComponentAsDouble(x, y, z, 0);
                out.voxels[idx++] = static_cast<float>(v);
            }
        }
    }

    if (!out.valid()) {
        error = wxT("Converted volume is invalid.");
        return false;
    }

    return true;
}

bool LoadDicomVolume(const wxString& directory, vtkSmartPointer<vtkImageData>& out, wxString& error) {
    // Convert wxString to std::string with safe lifetime management
    std::string dir_path = std::string(directory.mb_str());
    // Normalize path separators for VTK (use forward slashes)
    for (auto& ch : dir_path) {
        if (ch == '\\') ch = '/';
    }

    vtkNew<vtkDICOMImageReader> reader;
    try {
        reader->SetDirectoryName(dir_path.c_str());
        reader->Update();
    } catch (const std::exception& ex) {
        error = wxString::Format(wxT("VTK DICOM reader error: %s"), wxString::FromUTF8(ex.what()));
        return false;
    } catch (...) {
        error = wxT("VTK DICOM reader encountered an unknown error.");
        return false;
    }

    vtkImageData* image = reader->GetOutput();
    if (!image) {
        error = wxT("vtkDICOMImageReader returned null image.");
        return false;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        error = wxString::Format(wxT("Invalid DICOM dimensions: %d x %d x %d"), dims[0], dims[1], dims[2]);
        return false;
    }

    out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(image);
    return true;
}

int ClampIndex(int value, int max_value) {
    return std::max(0, std::min(value, std::max(0, max_value)));
}

int ParameterCountForTransformType(registration::TransformType type) {
    switch (type) {
        case registration::TransformType::kShift:
            return 3;
        case registration::TransformType::kShiftX:
            return 1;
        case registration::TransformType::kShiftY:
            return 1;
        case registration::TransformType::kShiftZ:
            return 1;
        case registration::TransformType::kShiftXZ:
            return 2;
        case registration::TransformType::kShiftRot:
            return 6;
        case registration::TransformType::kRotate:
            return 3;
        case registration::TransformType::kShiftRotMagn:
            return 7;
        case registration::TransformType::kShiftRotStretch:
            return 9;
        case registration::TransformType::kMagn:
            return 1;
        case registration::TransformType::kStretch:
            return 3;
    }
    return 6;
}

unsigned char NormalizeToByte(double value, const double range[2]) {
    const double min_value = range[0];
    const double max_value = range[1];
    if (max_value <= min_value) {
        return 0;
    }

    const double normalized = (value - min_value) / (max_value - min_value);
    const double clamped = std::max(0.0, std::min(1.0, normalized));
    return static_cast<unsigned char>(clamped * 255.0);
}

double SampleImageTrilinear(vtkImageData* image, double x, double y, double z, double fill_value = 0.0) {
    if (!image) {
        return fill_value;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return fill_value;
    }

    if (x < 0.0 || y < 0.0 || z < 0.0 || x > dims[0] - 1.0 || y > dims[1] - 1.0 || z > dims[2] - 1.0) {
        return fill_value;
    }

    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = std::min(x0 + 1, dims[0] - 1);
    const int y1 = std::min(y0 + 1, dims[1] - 1);
    const int z1 = std::min(z0 + 1, dims[2] - 1);

    const double dx = x - x0;
    const double dy = y - y0;
    const double dz = z - z0;

    const double c000 = image->GetScalarComponentAsDouble(x0, y0, z0, 0);
    const double c100 = image->GetScalarComponentAsDouble(x1, y0, z0, 0);
    const double c010 = image->GetScalarComponentAsDouble(x0, y1, z0, 0);
    const double c110 = image->GetScalarComponentAsDouble(x1, y1, z0, 0);
    const double c001 = image->GetScalarComponentAsDouble(x0, y0, z1, 0);
    const double c101 = image->GetScalarComponentAsDouble(x1, y0, z1, 0);
    const double c011 = image->GetScalarComponentAsDouble(x0, y1, z1, 0);
    const double c111 = image->GetScalarComponentAsDouble(x1, y1, z1, 0);

    const double c00 = c000 * (1.0 - dx) + c100 * dx;
    const double c10 = c010 * (1.0 - dx) + c110 * dx;
    const double c01 = c001 * (1.0 - dx) + c101 * dx;
    const double c11 = c011 * (1.0 - dx) + c111 * dx;
    const double c0 = c00 * (1.0 - dy) + c10 * dy;
    const double c1 = c01 * (1.0 - dy) + c11 * dy;
    return c0 * (1.0 - dz) + c1 * dz;
}

void TransformPoint(const registration::Transform& transform, double x, double y, double z, double& out_x, double& out_y, double& out_z) {
    out_x = transform.get(0, 0) * x + transform.get(0, 1) * y + transform.get(0, 2) * z + transform.get(0, 3);
    out_y = transform.get(1, 0) * x + transform.get(1, 1) * y + transform.get(1, 2) * z + transform.get(1, 3);
    out_z = transform.get(2, 0) * x + transform.get(2, 1) * y + transform.get(2, 2) * z + transform.get(2, 3);
}

void ExtractEulerZYXDeg(const registration::Transform& t, double& rx_deg, double& ry_deg, double& rz_deg) {
    const double r20 = t.get(2, 0);
    const double r21 = t.get(2, 1);
    const double r22 = t.get(2, 2);
    const double r10 = t.get(1, 0);
    const double r00 = t.get(0, 0);

    const double ry = std::asin(std::clamp(-r20, -1.0, 1.0));
    const double cy = std::cos(ry);

    double rx = 0.0;
    double rz = 0.0;
    if (std::fabs(cy) > 1e-6) {
        rx = std::atan2(r21, r22);
        rz = std::atan2(r10, r00);
    }

    constexpr double kRadToDeg = 57.29577951308232;
    rx_deg = rx * kRadToDeg;
    ry_deg = ry * kRadToDeg;
    rz_deg = rz * kRadToDeg;
}

float ComputePercentileSampled(const registration::Image3D& img, double percentile, int stride) {
    if (!img.valid()) {
        return 0.0f;
    }
    const int s = std::max(1, stride);
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(img.size.x / s + 1) *
                    static_cast<std::size_t>(img.size.y / s + 1) *
                    static_cast<std::size_t>(img.size.z / s + 1));
    for (int z = 0; z < img.size.z; z += s) {
        for (int y = 0; y < img.size.y; y += s) {
            for (int x = 0; x < img.size.x; x += s) {
                samples.push_back(img.at(x, y, z));
            }
        }
    }
    if (samples.empty()) {
        return 0.0f;
    }
    std::sort(samples.begin(), samples.end());
    const double p = std::clamp(percentile, 0.0, 100.0);
    const std::size_t idx = static_cast<std::size_t>(
        std::round((p / 100.0) * static_cast<double>(samples.size() - 1)));
    return samples[std::min(idx, samples.size() - 1)];
}

std::vector<double> BuildAxisProfileWindowed(
    const registration::Image3D& img,
    int axis,
    float low,
    float high,
    int stride) {
    const int s = std::max(1, stride);
    const int axis_len = (axis == 0) ? img.size.x : (axis == 1 ? img.size.y : img.size.z);
    std::vector<double> profile(static_cast<std::size_t>(axis_len), 0.0);
    if (!img.valid() || axis_len <= 0 || high <= low) {
        return profile;
    }

    for (int z = 0; z < img.size.z; z += s) {
        for (int y = 0; y < img.size.y; y += s) {
            for (int x = 0; x < img.size.x; x += s) {
                const float v = img.at(x, y, z);
                if (v <= 0.0f) {
                    continue;
                }
                const float clamped = std::max(low, std::min(v, high));
                const double norm = static_cast<double>(clamped - low) / static_cast<double>(high - low);
                const int idx = (axis == 0) ? x : (axis == 1 ? y : z);
                profile[static_cast<std::size_t>(idx)] += norm;
            }
        }
    }
    return profile;
}

int EstimateShiftFromProfiles(const std::vector<double>& reference_profile,
                              const std::vector<double>& online_profile,
                              int max_shift) {
    if (reference_profile.empty() || online_profile.empty() ||
        reference_profile.size() != online_profile.size()) {
        return 0;
    }
    const int n = static_cast<int>(reference_profile.size());
    const int lim = std::max(1, max_shift);

    int best_shift = 0;
    double best_score = -std::numeric_limits<double>::infinity();

    for (int shift = -lim; shift <= lim; ++shift) {
        int start = 0;
        int end = n;
        if (shift < 0) {
            start = -shift;
        } else if (shift > 0) {
            end = n - shift;
        }
        if (end - start < 8) {
            continue;
        }

        double mean_a = 0.0;
        double mean_b = 0.0;
        int count = 0;
        for (int i = start; i < end; ++i) {
            mean_a += reference_profile[static_cast<std::size_t>(i)];
            mean_b += online_profile[static_cast<std::size_t>(i + shift)];
            ++count;
        }
        if (count <= 1) {
            continue;
        }
        mean_a /= static_cast<double>(count);
        mean_b /= static_cast<double>(count);

        double num = 0.0;
        double den_a = 0.0;
        double den_b = 0.0;
        for (int i = start; i < end; ++i) {
            const double a = reference_profile[static_cast<std::size_t>(i)] - mean_a;
            const double b = online_profile[static_cast<std::size_t>(i + shift)] - mean_b;
            num += a * b;
            den_a += a * a;
            den_b += b * b;
        }
        const double den = std::sqrt(den_a * den_b);
        if (den <= 1e-12) {
            continue;
        }
        const double score = num / den;
        if (score > best_score) {
            best_score = score;
            best_shift = shift;
        }
    }

    return best_shift;
}

std::array<int, 3> EstimateInitialTranslationShift(const registration::Image3D& reference,
                                                   const registration::Image3D& online,
                                                   int max_shift_voxels,
                                                   int sampling_stride) {
    const float ref_p98 = ComputePercentileSampled(reference, 98.0, sampling_stride);
    const float on_p98 = ComputePercentileSampled(online, 98.0, sampling_stride);
    const float window_high = std::max(ref_p98, on_p98);
    const float window_low = 0.03f * window_high;

    std::array<int, 3> shift = {0, 0, 0};
    for (int axis = 0; axis < 3; ++axis) {
        const auto ref_profile = BuildAxisProfileWindowed(reference, axis, window_low, window_high, sampling_stride);
        const auto on_profile = BuildAxisProfileWindowed(online, axis, window_low, window_high, sampling_stride);
        shift[axis] = EstimateShiftFromProfiles(ref_profile, on_profile, max_shift_voxels);
    }
    return shift;
}

// RegistrationThread: 在后台线程中执行配准
class RegistrationThread {
public:
    struct Params {
        wxString ref_dir;
        wxString online_dir;
        MatchAlgorithm algorithm = MatchAlgorithm::kGreyValue;
    };

    struct Result {
        bool success = false;
        wxString error_msg;
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        double rx = 0.0, ry = 0.0, rz = 0.0;
        int eval_count = 0;
        double start_cf = 0.0, end_cf = 0.0;
        wxString info;
        wxString algorithm_label;
        registration::Transform transform;
    };

    static Result RunRegistration(const Params& params, wxEvtHandler* callback) {
        Result result;

        // 加载参考卷
        const auto t_start = std::chrono::steady_clock::now();
        auto elapsed_sec = [&t_start]() -> double {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        };

        const size_t ref_file_count = CountDicomFilesInDir(params.ref_dir);
        const size_t online_file_count = CountDicomFilesInDir(params.online_dir);
        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("[Registration] Reference DICOM: %zu files  |  Online DICOM: %zu files"),
                ref_file_count, online_file_count));
            callback->AddPendingEvent(event);
        }

        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxT("[Registration] Loading reference volume (VTK DICOM)..."));
            callback->AddPendingEvent(event);
        }

        registration::Image3D reference;
        std::array<double, 3> ref_spacing = {1.0, 1.0, 1.0};
        std::array<double, 3> ref_origin = {0.0, 0.0, 0.0};
        wxString load_error;
        if (!LoadDicomVolumeAsImage3D(params.ref_dir, reference, load_error, &ref_spacing, &ref_origin)) {
            result.error_msg = wxString::Format(wxT("Failed to load reference: %s"), load_error);
            if (callback) {
                wxCommandEvent event(wxEVT_REG_ERROR);
                event.SetString(result.error_msg);
                callback->AddPendingEvent(event);
            }
            return result;
        }

        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("[Registration] Reference loaded: %d x %d x %d voxels  (%.1f s)"),
                reference.size.x, reference.size.y, reference.size.z, elapsed_sec()));
            callback->AddPendingEvent(event);
        }

        // 加载在线卷
        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxT("[Registration] Loading online volume (VTK DICOM)..."));
            callback->AddPendingEvent(event);
        }

        registration::Image3D online;
        std::array<double, 3> online_spacing = {1.0, 1.0, 1.0};
        std::array<double, 3> online_origin = {0.0, 0.0, 0.0};
        if (!LoadDicomVolumeAsImage3D(params.online_dir, online, load_error, &online_spacing, &online_origin)) {
            result.error_msg = wxString::Format(wxT("Failed to load online: %s"), load_error);
            if (callback) {
                wxCommandEvent event(wxEVT_REG_ERROR);
                event.SetString(result.error_msg);
                callback->AddPendingEvent(event);
            }
            return result;
        }

        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("[Registration] Online loaded: %d x %d x %d voxels  (total load %.1f s)"),
                online.size.x, online.size.y, online.size.z, elapsed_sec()));
            callback->AddPendingEvent(event);
        }

        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("[Registration] Geometry ref spacing=(%.4f, %.4f, %.4f) origin=(%.4f, %.4f, %.4f); online spacing=(%.4f, %.4f, %.4f) origin=(%.4f, %.4f, %.4f)"),
                ref_spacing[0], ref_spacing[1], ref_spacing[2],
                ref_origin[0], ref_origin[1], ref_origin[2],
                online_spacing[0], online_spacing[1], online_spacing[2],
                online_origin[0], online_origin[1], online_origin[2]));
            callback->AddPendingEvent(event);
        }

        const bool geometry_mismatch =
            (std::fabs(ref_spacing[0] - online_spacing[0]) > 1e-3) ||
            (std::fabs(ref_spacing[1] - online_spacing[1]) > 1e-3) ||
            (std::fabs(ref_spacing[2] - online_spacing[2]) > 1e-3) ||
            (std::fabs(ref_origin[0] - online_origin[0]) > 1e-2) ||
            (std::fabs(ref_origin[1] - online_origin[1]) > 1e-2) ||
            (std::fabs(ref_origin[2] - online_origin[2]) > 1e-2);

        if (geometry_mismatch && callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxT("[Registration] Warning: ref/online DICOM geometry differs; current optimizer uses voxel-index space, which can bias translation estimates."));
            callback->AddPendingEvent(event);
        }

        // 运行配准 (始终使用6个自由度: 3自由度平移 + 3自由度旋转)
        const wxString algorithm_label = MatchAlgorithmToLabel(params.algorithm);
        const MatchAlgorithmConfig algorithm_config = GetMatchAlgorithmConfig(params.algorithm);
        result.algorithm_label = algorithm_label;
        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("Running 6-DOF registration (registration::Registration), algorithm=%s ..."),
                algorithm_label));
            callback->AddPendingEvent(event);
        }

        struct StageOutcome {
            bool success = false;
            bool timed_out = false;
            bool saw_improvement = false;
            int eval_count = 0;
            double start_cf = 0.0;
            double end_cf = 0.0;
            wxString info;
            wxString error;
            registration::Transform transform;
        };

        auto run_stage = [callback](
                             registration::Registration& reg,
                             const wxString& stage_name,
                             const wxString& algorithm_label_local,
                             double timeout_sec,
                             double no_improve_timeout_sec,
                             int sample_stride,
                             registration::TransformType type,
                             double tol,
                             double offset) -> StageOutcome {
            // 单阶段执行器：统一封装“启动优化 + 监控 + 早停 + 结果收集”。
            // 这样每个算法只关注阶段编排，不重复写监控细节。
            StageOutcome out;
            const int eval_gate = ParameterCountForTransformType(type) + 1;
            const auto t_opt_start = std::chrono::steady_clock::now();
            auto opt_elapsed = [&t_opt_start]() -> double {
                return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_opt_start).count();
            };

            auto keep_running = std::make_shared<std::atomic<bool>>(true);
            auto timed_out = std::make_shared<std::atomic<bool>>(false);
            auto saw_improvement = std::make_shared<std::atomic<bool>>(false);
            auto last_improve_sec = std::make_shared<std::atomic<double>>(0.0);

            reg.set_sample_stride(sample_stride);

            reg.set_on_lower_func_val([callback, &opt_elapsed, stage_name, saw_improvement, last_improve_sec](double cf_value, int evals, const std::string& info) {
                saw_improvement->store(true);
                last_improve_sec->store(opt_elapsed());
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Better cost: %.6f  eval=%d  t=%.1fs  (%s)"),
                        stage_name, cf_value, evals, opt_elapsed(), wxString::FromUTF8(info)));
                    callback->AddPendingEvent(event);
                }
            });

            auto monitor_thread = std::thread([callback, keep_running, timed_out, &reg, &opt_elapsed, timeout_sec, no_improve_timeout_sec, stage_name, last_improve_sec, eval_gate]() {
                constexpr int kTickSec = 5;
                int ticks = 0;
                while (keep_running->load()) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (!keep_running->load()) break;
                    const double t = opt_elapsed();
                    if (static_cast<int>(t) % kTickSec == 0 && static_cast<int>(t) / kTickSec > ticks) {
                        ticks = static_cast<int>(t) / kTickSec;
                        if (callback) {
                            wxCommandEvent ev(wxEVT_REG_LOG);
                            ev.SetString(wxString::Format(
                                wxT("[Registration] [%s] Optimization running... t=%.0fs / %.0fs (timeout)"),
                                stage_name, t, timeout_sec));
                            callback->AddPendingEvent(ev);
                        }
                    }
                    if (t >= timeout_sec && !timed_out->load()) {
                        timed_out->store(true);
                        if (callback) {
                            wxCommandEvent ev(wxEVT_REG_LOG);
                            ev.SetString(wxString::Format(
                                wxT("[Registration] [%s] Timeout reached (%.0fs) - stopping with best result so far"),
                                stage_name, timeout_sec));
                            callback->AddPendingEvent(ev);
                        }
                        reg.stop();
                        break;
                    }

                    if (no_improve_timeout_sec > 0.0 && reg.eval_count() >= eval_gate) {
                        // 避免刚起步时误判“无改进”：至少达到一个最小评估门槛再启用静默超时。
                        const double silent_sec = t - last_improve_sec->load();
                        if (silent_sec >= no_improve_timeout_sec && !timed_out->load()) {
                            if (callback) {
                                wxCommandEvent ev(wxEVT_REG_LOG);
                                ev.SetString(wxString::Format(
                                    wxT("[Registration] [%s] No improvement for %.0fs - early stop with best result so far"),
                                    stage_name, no_improve_timeout_sec));
                                callback->AddPendingEvent(ev);
                            }
                            reg.stop();
                            break;
                        }
                    }
                }
            });

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                if (type == registration::TransformType::kShift) {
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Optimization started - timeout %.0fs, algorithm=%s, tol=%.4f, initial_step=%.3f (Shift: T step=%.3f mm)"),
                        stage_name, timeout_sec, algorithm_label_local, tol, offset, offset / 10.0));
                } else if (type == registration::TransformType::kShiftX) {
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Optimization started - timeout %.0fs, algorithm=%s, tol=%.4f, initial_step=%.3f (ShiftX: TX step=%.3f mm)"),
                        stage_name, timeout_sec, algorithm_label_local, tol, offset, offset / 10.0));
                } else if (type == registration::TransformType::kShiftY) {
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Optimization started - timeout %.0fs, algorithm=%s, tol=%.4f, initial_step=%.3f (ShiftY: TY step=%.3f mm)"),
                        stage_name, timeout_sec, algorithm_label_local, tol, offset, offset / 10.0));
                } else if (type == registration::TransformType::kShiftZ) {
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Optimization started - timeout %.0fs, algorithm=%s, tol=%.4f, initial_step=%.3f (ShiftZ: TZ step=%.3f mm)"),
                        stage_name, timeout_sec, algorithm_label_local, tol, offset, offset / 10.0));
                } else if (type == registration::TransformType::kShiftXZ) {
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Optimization started - timeout %.0fs, algorithm=%s, tol=%.4f, initial_step=%.3f (ShiftXZ: TX/TZ step=%.3f mm)"),
                        stage_name, timeout_sec, algorithm_label_local, tol, offset, offset / 10.0));
                } else {
                    event.SetString(wxString::Format(
                        wxT("[Registration] [%s] Optimization started - timeout %.0fs, algorithm=%s, tol=%.4f, initial_step=%.3f (ShiftRot: T step=%.3f mm, R step=%.3f deg)"),
                        stage_name, timeout_sec, algorithm_label_local, tol, offset, offset / 10.0, offset));
                }
                if (sample_stride > 1) {
                    event.SetString(event.GetString() + wxString::Format(wxT(" [sample_stride=%d]"), sample_stride));
                }
                callback->AddPendingEvent(event);
            }

            try {
                out.transform = reg.run();
                out.success = true;
            } catch (const std::exception& ex) {
                out.error = wxString::Format(wxT("[%s] Registration exception: %s"), stage_name, wxString::FromUTF8(ex.what()));
            } catch (...) {
                out.error = wxString::Format(wxT("[%s] Registration failed with unknown exception."), stage_name);
            }

            keep_running->store(false);
            monitor_thread.join();

            out.timed_out = timed_out->load();
            out.saw_improvement = saw_improvement->load();
            out.eval_count = reg.eval_count();
            out.start_cf = reg.start_cf_value();
            out.end_cf = reg.end_cf_value();
            out.info = wxString::FromUTF8(reg.information());
            return out;
        };

        constexpr double kTimeoutSec = 120.0;

        int total_evals = 0;
        double total_start_cf = 0.0;
        double total_end_cf = 0.0;
        bool any_timed_out = false;
        bool any_improvement = false;

        registration::Transform reg_result;
        wxString final_info;

        registration::Transform initial_guess_transform;
        bool has_initial_guess = false;

        if (params.algorithm == MatchAlgorithm::kSeed) {
            // Seed：先平移后 6DOF。
            // 目标：用 NMI 快速收敛到合理平移，再按需加旋转精修。
            const std::array<int, 3> coarse_shift = EstimateInitialTranslationShift(reference, online, 30, 2);
            initial_guess_transform.make_translation(
                static_cast<float>(coarse_shift[0]),
                static_cast<float>(coarse_shift[1]),
                static_cast<float>(coarse_shift[2]));
            has_initial_guess = true;

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxString::Format(
                    wxT("[Registration] Seed initial translation estimate (profile NCC): T=(%d, %d, %d) mm"),
                    coarse_shift[0], coarse_shift[1], coarse_shift[2]));
                callback->AddPendingEvent(event);
            }

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxT("[Registration] Seed staged search enabled: Stage-1 translation-only, then Stage-2 full 6-DOF refinement."));
                callback->AddPendingEvent(event);
            }

            registration::Registration reg_stage1;
            reg_stage1.set_scan1(reference);
            reg_stage1.set_scan2(online);
            reg_stage1.set_transform_type(registration::TransformType::kShift);
            if (has_initial_guess) {
                reg_stage1.set_adjust2(initial_guess_transform);
            }
            reg_stage1.set_metric(algorithm_config.metric);
            reg_stage1.set_exclude_zeros(true);
            reg_stage1.set_post_process(true);
            reg_stage1.set_param1(algorithm_config.tolerance);
            reg_stage1.set_param2(std::max(algorithm_config.offset, 8.0));

            StageOutcome stage1 = run_stage(
                reg_stage1,
                wxT("Stage-1 Shift"),
                algorithm_label,
                45.0,
                6.0,
                4,
                registration::TransformType::kShift,
                algorithm_config.tolerance,
                std::max(algorithm_config.offset, 8.0));

            if (!stage1.success) {
                result.error_msg = stage1.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

            total_evals += stage1.eval_count;
            total_start_cf = stage1.start_cf;
            total_end_cf = stage1.end_cf;
            any_timed_out = any_timed_out || stage1.timed_out;
            any_improvement = any_improvement || stage1.saw_improvement;

            float stage1_tx = 0.0f, stage1_ty = 0.0f, stage1_tz = 0.0f;
            stage1.transform.get_translation(stage1_tx, stage1_ty, stage1_tz);

            // For Seed, if stage-1 already converged near pure-translation solution,
            // skip expensive 6-DOF stage-2.
            const bool skip_seed_stage2 =
                stage1.end_cf <= 0.70 &&
                std::fabs(stage1_tx) <= 1.0f &&
                std::fabs(stage1_tz) <= 1.5f;

            if (skip_seed_stage2) {
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxString::Format(
                        wxT("[Registration] Seed Stage-2 skipped: Stage-1 already stable (cost=%.6f, Tx=%.3f, Tz=%.3f)."),
                        stage1.end_cf, stage1_tx, stage1_tz));
                    callback->AddPendingEvent(event);
                }

                reg_result = stage1.transform;
                final_info = wxString::Format(wxT("stage1{%s}; skipped_stage2=1"), stage1.info);
            } else {

                registration::Registration reg_stage2;
                reg_stage2.set_scan1(reference);
                reg_stage2.set_scan2(online);
                reg_stage2.set_transform_type(registration::TransformType::kShiftRot);
                reg_stage2.set_adjust2(stage1.transform);
                reg_stage2.set_metric(algorithm_config.metric);
                reg_stage2.set_exclude_zeros(true);
                reg_stage2.set_post_process(true);
                reg_stage2.set_param1(algorithm_config.tolerance);
                reg_stage2.set_param2(algorithm_config.offset);

                StageOutcome stage2 = run_stage(
                    reg_stage2,
                    wxT("Stage-2 ShiftRot"),
                    algorithm_label,
                    60.0,
                    12.0,
                    2,
                    registration::TransformType::kShiftRot,
                    algorithm_config.tolerance,
                    algorithm_config.offset);

                if (!stage2.success) {
                    result.error_msg = stage2.error;
                    if (callback) {
                        wxCommandEvent event(wxEVT_REG_ERROR);
                        event.SetString(result.error_msg);
                        callback->AddPendingEvent(event);
                    }
                    return result;
                }

                total_evals += stage2.eval_count;
                total_end_cf = stage2.end_cf;
                any_timed_out = any_timed_out || stage2.timed_out;
                any_improvement = any_improvement || stage2.saw_improvement;

                reg_result = stage2.transform;
                final_info = wxString::Format(wxT("stage1{%s}; stage2{%s}"), stage1.info, stage2.info);
            }
        } else if (params.algorithm == MatchAlgorithm::kBone) {
            // Bone：同样采用两阶段策略，但阈值更偏向结构强度图的稳定性。
            // Stage-1 若已接近完美（低成本 + 极小平移），则跳过 Stage-2。
            const std::array<int, 3> coarse_shift = EstimateInitialTranslationShift(reference, online, 30, 2);
            initial_guess_transform.make_translation(
                static_cast<float>(coarse_shift[0]),
                static_cast<float>(coarse_shift[1]),
                static_cast<float>(coarse_shift[2]));
            has_initial_guess = true;

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxString::Format(
                    wxT("[Registration] Bone initial translation estimate (profile NCC): T=(%d, %d, %d) mm"),
                    coarse_shift[0], coarse_shift[1], coarse_shift[2]));
                callback->AddPendingEvent(event);
            }

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxT("[Registration] Bone staged search enabled: Stage-1 translation-only, then Stage-2 full 6-DOF refinement."));
                callback->AddPendingEvent(event);
            }

            registration::Registration reg_stage1;
            reg_stage1.set_scan1(reference);
            reg_stage1.set_scan2(online);
            reg_stage1.set_transform_type(registration::TransformType::kShift);
            if (has_initial_guess) {
                reg_stage1.set_adjust2(initial_guess_transform);
            }
            reg_stage1.set_metric(algorithm_config.metric);
            reg_stage1.set_exclude_zeros(true);
            reg_stage1.set_post_process(true);
            reg_stage1.set_param1(algorithm_config.tolerance);
            reg_stage1.set_param2(std::max(algorithm_config.offset, 10.0));

            StageOutcome stage1 = run_stage(
                reg_stage1,
                wxT("Stage-1 Shift"),
                algorithm_label,
                30.0,
                4.0,
                4,
                registration::TransformType::kShift,
                algorithm_config.tolerance,
                std::max(algorithm_config.offset, 10.0));

            if (!stage1.success) {
                result.error_msg = stage1.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

            total_evals += stage1.eval_count;
            total_start_cf = stage1.start_cf;
            total_end_cf = stage1.end_cf;
            any_timed_out = any_timed_out || stage1.timed_out;
            any_improvement = any_improvement || stage1.saw_improvement;

            float stage1_tx = 0.0f, stage1_ty = 0.0f, stage1_tz = 0.0f;
            stage1.transform.get_translation(stage1_tx, stage1_ty, stage1_tz);
            const double bone_stage1_max_abs_t = std::max({
                std::fabs(static_cast<double>(stage1_tx)),
                std::fabs(static_cast<double>(stage1_ty)),
                std::fabs(static_cast<double>(stage1_tz))});

            const bool skip_bone_stage2 =
                stage1.end_cf <= 1.0 && bone_stage1_max_abs_t <= 0.5;

            if (skip_bone_stage2) {
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxString::Format(
                        wxT("[Registration] Bone Stage-2 skipped: Stage-1 already near-perfect (cost=%.6f, |T|max=%.3f mm)."),
                        stage1.end_cf,
                        bone_stage1_max_abs_t));
                    callback->AddPendingEvent(event);
                }

                reg_result = stage1.transform;
                final_info = wxString::Format(wxT("stage1{%s}; skipped_stage2=1"), stage1.info);
            } else {

            registration::Registration reg_stage2;
            reg_stage2.set_scan1(reference);
            reg_stage2.set_scan2(online);
            reg_stage2.set_transform_type(registration::TransformType::kShiftRot);
            reg_stage2.set_adjust2(stage1.transform);
            reg_stage2.set_metric(algorithm_config.metric);
            reg_stage2.set_exclude_zeros(true);
            reg_stage2.set_post_process(true);
            reg_stage2.set_param1(algorithm_config.tolerance);
            reg_stage2.set_param2(algorithm_config.offset);

            StageOutcome stage2 = run_stage(
                reg_stage2,
                wxT("Stage-2 ShiftRot"),
                algorithm_label,
                30.0,
                6.0,
                2,
                registration::TransformType::kShiftRot,
                algorithm_config.tolerance,
                algorithm_config.offset);

            if (!stage2.success) {
                result.error_msg = stage2.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

            total_evals += stage2.eval_count;
            total_end_cf = stage2.end_cf;
            any_timed_out = any_timed_out || stage2.timed_out;
            any_improvement = any_improvement || stage2.saw_improvement;

            reg_result = stage2.transform;
            final_info = wxString::Format(wxT("stage1{%s}; stage2{%s}"), stage1.info, stage2.info);
            }
        } else if (params.algorithm == MatchAlgorithm::kGreyValue) {
            // Grey value：最细粒度的多阶段策略。
            // Stage-1/2 完成全局对齐，Stage-3~6 用单轴/双轴局部微调压低残差。
            const std::array<int, 3> coarse_shift = EstimateInitialTranslationShift(reference, online, 30, 2);
            initial_guess_transform.make_translation(
                static_cast<float>(coarse_shift[0]),
                static_cast<float>(coarse_shift[1]),
                static_cast<float>(coarse_shift[2]));
            has_initial_guess = true;

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxString::Format(
                    wxT("[Registration] Grey value initial translation estimate (profile NCC): T=(%d, %d, %d) mm"),
                    coarse_shift[0], coarse_shift[1], coarse_shift[2]));
                callback->AddPendingEvent(event);
            }

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxT("[Registration] Grey value staged search enabled: Stage-1 translation-only, then Stage-2 full 6-DOF refinement."));
                callback->AddPendingEvent(event);
            }

            registration::Registration reg_stage1;
            reg_stage1.set_scan1(reference);
            reg_stage1.set_scan2(online);
            reg_stage1.set_transform_type(registration::TransformType::kShift);
            if (has_initial_guess) {
                reg_stage1.set_adjust2(initial_guess_transform);
            }
            reg_stage1.set_metric(algorithm_config.metric);
            reg_stage1.set_exclude_zeros(true);
            reg_stage1.set_post_process(true);
            reg_stage1.set_param1(algorithm_config.tolerance);
            reg_stage1.set_param2(std::max(algorithm_config.offset, 12.0));

            StageOutcome stage1 = run_stage(
                reg_stage1,
                wxT("Stage-1 Shift"),
                algorithm_label,
                90.0,
                12.0,
                4,
                registration::TransformType::kShift,
                algorithm_config.tolerance,
                std::max(algorithm_config.offset, 12.0));

            if (!stage1.success) {
                result.error_msg = stage1.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

            total_evals += stage1.eval_count;
            total_start_cf = stage1.start_cf;
            total_end_cf = stage1.end_cf;
            any_timed_out = any_timed_out || stage1.timed_out;
            any_improvement = any_improvement || stage1.saw_improvement;

            float stage1_tx = 0.0f, stage1_ty = 0.0f, stage1_tz = 0.0f;
            stage1.transform.get_translation(stage1_tx, stage1_ty, stage1_tz);
            const double grey_stage1_max_abs_t = std::max({
                std::fabs(static_cast<double>(stage1_tx)),
                std::fabs(static_cast<double>(stage1_ty)),
                std::fabs(static_cast<double>(stage1_tz))});
            const bool skip_grey_stage2 =
                stage1.end_cf <= 0.0025 &&
                grey_stage1_max_abs_t <= 0.5;

            if (skip_grey_stage2) {
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxString::Format(
                        wxT("[Registration] Grey value Stage-2 skipped: Stage-1 already near-perfect (cost=%.6f, |T|max=%.3f mm)."),
                        stage1.end_cf,
                        grey_stage1_max_abs_t));
                    callback->AddPendingEvent(event);
                }

                reg_result = stage1.transform;
                final_info = wxString::Format(wxT("stage1{%s}; skipped_stage2=1"), stage1.info);
            } else {

            registration::Registration reg_stage2;
            reg_stage2.set_scan1(reference);
            reg_stage2.set_scan2(online);
            reg_stage2.set_transform_type(registration::TransformType::kShiftRot);
            reg_stage2.set_adjust2(stage1.transform);
            reg_stage2.set_metric(algorithm_config.metric);
            reg_stage2.set_exclude_zeros(true);
            reg_stage2.set_post_process(true);
            reg_stage2.set_param1(algorithm_config.tolerance);
            reg_stage2.set_param2(algorithm_config.offset);

            StageOutcome stage2 = run_stage(
                reg_stage2,
                wxT("Stage-2 ShiftRot"),
                algorithm_label,
                kTimeoutSec,
                15.0,
                4,
                registration::TransformType::kShiftRot,
                algorithm_config.tolerance,
                algorithm_config.offset);

            if (!stage2.success) {
                result.error_msg = stage2.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

            total_evals += stage2.eval_count;
            total_end_cf = stage2.end_cf;
            any_timed_out = any_timed_out || stage2.timed_out;
            any_improvement = any_improvement || stage2.saw_improvement;

            float stage1b_tx = 0.0f, stage1b_ty = 0.0f, stage1b_tz = 0.0f;
            float stage2_tx = 0.0f, stage2_ty = 0.0f, stage2_tz = 0.0f;
            stage1.transform.get_translation(stage1b_tx, stage1b_ty, stage1b_tz);
            stage2.transform.get_translation(stage2_tx, stage2_ty, stage2_tz);

            double stage1_rx = 0.0, stage1_ry = 0.0, stage1_rz = 0.0;
            double stage2_rx = 0.0, stage2_ry = 0.0, stage2_rz = 0.0;
            ExtractEulerZYXDeg(stage1.transform, stage1_rx, stage1_ry, stage1_rz);
            ExtractEulerZYXDeg(stage2.transform, stage2_rx, stage2_ry, stage2_rz);

            const double stage2_cf_gain = stage1.end_cf - stage2.end_cf;
            const double stage2_cf_gain_rel =
                (std::fabs(stage1.end_cf) > 1e-9) ? (stage2_cf_gain / std::fabs(stage1.end_cf)) : 0.0;
            const double stage2_shift_delta = std::max({
                std::fabs(static_cast<double>(stage2_tx - stage1b_tx)),
                std::fabs(static_cast<double>(stage2_ty - stage1b_ty)),
                std::fabs(static_cast<double>(stage2_tz - stage1b_tz))});
            const double stage2_rot_delta = std::max({
                std::fabs(stage2_rx - stage1_rx),
                std::fabs(stage2_ry - stage1_ry),
                std::fabs(stage2_rz - stage1_rz)});

            const bool skip_late_refinement =
                stage1.end_cf <= 0.02 &&
                stage2_cf_gain_rel <= 0.02 &&
                stage2_shift_delta <= 0.5 &&
                stage2_rot_delta <= 0.1;

            if (skip_late_refinement) {
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxString::Format(
                        wxT("[Registration] Grey value late refinement skipped: Stage-2 already stable (rel_gain=%.4f, dT<=%.3f mm, dR<=%.3f deg)."),
                        stage2_cf_gain_rel,
                        stage2_shift_delta,
                        stage2_rot_delta));
                    callback->AddPendingEvent(event);
                }

                reg_result = stage2.transform;
                final_info = wxString::Format(wxT("stage1{%s}; stage2{%s}; skipped_late_refinement=1"), stage1.info, stage2.info);
            } else {

                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxT("[Registration] Grey value Stage-3 enabled: Y-axis only refinement from Stage-2 result."));
                    callback->AddPendingEvent(event);
                }

                // Stage-3: 仅 TY 微调，用于快速捕捉纵向主偏差。

                registration::Registration reg_stage3;
                reg_stage3.set_scan1(reference);
                reg_stage3.set_scan2(online);
                reg_stage3.set_transform_type(registration::TransformType::kShiftY);
                reg_stage3.set_adjust2(stage2.transform);
                reg_stage3.set_metric(algorithm_config.metric);
                reg_stage3.set_exclude_zeros(true);
                reg_stage3.set_post_process(true);
                reg_stage3.set_param1(algorithm_config.tolerance);
                reg_stage3.set_param2(6.0);

                StageOutcome stage3 = run_stage(
                    reg_stage3,
                    wxT("Stage-3 ShiftY"),
                    algorithm_label,
                    90.0,
                    10.0,
                    2,
                    registration::TransformType::kShiftY,
                    algorithm_config.tolerance,
                    6.0);

                if (!stage3.success) {
                    result.error_msg = stage3.error;
                    if (callback) {
                        wxCommandEvent event(wxEVT_REG_ERROR);
                        event.SetString(result.error_msg);
                        callback->AddPendingEvent(event);
                    }
                    return result;
                }

                total_evals += stage3.eval_count;
                total_end_cf = stage3.end_cf;
                any_timed_out = any_timed_out || stage3.timed_out;
                any_improvement = any_improvement || stage3.saw_improvement;

                if (callback) {
                    wxCommandEvent event(wxEVT_REG_LOG);
                    event.SetString(wxT("[Registration] Grey value Stage-4 enabled: X/Z fine refinement from Stage-3 result."));
                    callback->AddPendingEvent(event);
                }

                // Stage-4: 联合 TX/TZ 微调，补偿横向与前后残差。

            registration::Registration reg_stage4;
            reg_stage4.set_scan1(reference);
            reg_stage4.set_scan2(online);
            reg_stage4.set_transform_type(registration::TransformType::kShiftXZ);
            reg_stage4.set_adjust2(stage3.transform);
            reg_stage4.set_metric(algorithm_config.metric);
            reg_stage4.set_exclude_zeros(true);
            reg_stage4.set_post_process(true);
            reg_stage4.set_param1(algorithm_config.tolerance);
            reg_stage4.set_param2(4.0);

                StageOutcome stage4 = run_stage(
                    reg_stage4,
                    wxT("Stage-4 ShiftXZ"),
                    algorithm_label,
                    60.0,
                    10.0,
                    2,
                    registration::TransformType::kShiftXZ,
                    algorithm_config.tolerance,
                    4.0);

                if (!stage4.success) {
                result.error_msg = stage4.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

                total_evals += stage4.eval_count;
                total_end_cf = stage4.end_cf;
                any_timed_out = any_timed_out || stage4.timed_out;
                any_improvement = any_improvement || stage4.saw_improvement;

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxT("[Registration] Grey value Stage-5 enabled: X-axis micro-refinement from Stage-4 result."));
                callback->AddPendingEvent(event);
            }

            // Stage-5: 单独 X 轴微调，进一步抑制局部耦合误差。

            registration::Registration reg_stage5;
            reg_stage5.set_scan1(reference);
            reg_stage5.set_scan2(online);
            reg_stage5.set_transform_type(registration::TransformType::kShiftX);
            reg_stage5.set_adjust2(stage4.transform);
            reg_stage5.set_metric(algorithm_config.metric);
            reg_stage5.set_exclude_zeros(true);
            reg_stage5.set_post_process(true);
            reg_stage5.set_param1(algorithm_config.tolerance);
            reg_stage5.set_param2(2.0);

                StageOutcome stage5 = run_stage(
                    reg_stage5,
                    wxT("Stage-5 ShiftXLocal"),
                    algorithm_label,
                    20.0,
                    8.0,
                    1,
                    registration::TransformType::kShiftX,
                    algorithm_config.tolerance,
                    2.0);

                if (!stage5.success) {
                result.error_msg = stage5.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

                total_evals += stage5.eval_count;
                total_end_cf = stage5.end_cf;
                any_timed_out = any_timed_out || stage5.timed_out;
                any_improvement = any_improvement || stage5.saw_improvement;

            if (callback) {
                wxCommandEvent event(wxEVT_REG_LOG);
                event.SetString(wxT("[Registration] Grey value Stage-6 enabled: Z-axis micro-refinement from Stage-5 result."));
                callback->AddPendingEvent(event);
            }

            // Stage-6: 单独 Z 轴微调，作为最终收尾阶段。

            registration::Registration reg_stage6;
            reg_stage6.set_scan1(reference);
            reg_stage6.set_scan2(online);
            reg_stage6.set_transform_type(registration::TransformType::kShiftZ);
            reg_stage6.set_adjust2(stage5.transform);
            reg_stage6.set_metric(algorithm_config.metric);
            reg_stage6.set_exclude_zeros(true);
            reg_stage6.set_post_process(true);
            reg_stage6.set_param1(algorithm_config.tolerance);
            reg_stage6.set_param2(2.0);

                StageOutcome stage6 = run_stage(
                    reg_stage6,
                    wxT("Stage-6 ShiftZLocal"),
                    algorithm_label,
                    15.0,
                    6.0,
                    1,
                    registration::TransformType::kShiftZ,
                    algorithm_config.tolerance,
                    2.0);

                if (!stage6.success) {
                result.error_msg = stage6.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

                total_evals += stage6.eval_count;
                total_end_cf = stage6.end_cf;
                any_timed_out = any_timed_out || stage6.timed_out;
                any_improvement = any_improvement || stage6.saw_improvement;

                reg_result = stage6.transform;
                final_info = wxString::Format(wxT("stage1{%s}; stage2{%s}; stage3{%s}; stage4{%s}; stage5{%s}; stage6{%s}"), stage1.info, stage2.info, stage3.info, stage4.info, stage5.info, stage6.info);
            }
            }
        } else {
            registration::Registration reg;
            reg.set_scan1(reference);
            reg.set_scan2(online);
            reg.set_transform_type(registration::TransformType::kShiftRot);
            static_assert(sizeof(reg) > 0, "6-DOF (dx,dy,dz,rx,ry,rz) enforced via kShiftRot");
            reg.set_metric(algorithm_config.metric);
            reg.set_exclude_zeros(true);
            reg.set_post_process(true);
            reg.set_param1(algorithm_config.tolerance);
            reg.set_param2(algorithm_config.offset);

            StageOutcome stage = run_stage(
                reg,
                wxT("Stage-1 ShiftRot"),
                algorithm_label,
                kTimeoutSec,
                60.0,
                2,
                registration::TransformType::kShiftRot,
                algorithm_config.tolerance,
                algorithm_config.offset);

            if (!stage.success) {
                result.error_msg = stage.error;
                if (callback) {
                    wxCommandEvent event(wxEVT_REG_ERROR);
                    event.SetString(result.error_msg);
                    callback->AddPendingEvent(event);
                }
                return result;
            }

            total_evals = stage.eval_count;
            total_start_cf = stage.start_cf;
            total_end_cf = stage.end_cf;
            any_timed_out = stage.timed_out;
            any_improvement = stage.saw_improvement;
            reg_result = stage.transform;
            final_info = stage.info;
        }

        // 提取6个自由度的结果: 平移(tx,ty,tz) + 旋转欧拉角(rx,ry,rz)
        reg_result.get_translation(result.tx, result.ty, result.tz);
        ExtractEulerZYXDeg(reg_result, result.rx, result.ry, result.rz);
        result.eval_count = total_evals;
        result.start_cf = total_start_cf;
        result.end_cf = total_end_cf;
        result.info = final_info;
        result.transform = reg_result;
        result.success = true;

        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("[Registration] Registration complete - %d evals, cost %.6f -> %.6f"),
                result.eval_count, result.start_cf, result.end_cf));
            callback->AddPendingEvent(event);
        }

        if (callback) {
            wxCommandEvent event(wxEVT_REG_LOG);
            event.SetString(wxString::Format(
                wxT("[Registration] Diagnostics: timed_out=%d, saw_improvement=%d, result T=(%.3f, %.3f, %.3f), R=(%.3f, %.3f, %.3f)"),
                any_timed_out ? 1 : 0,
                any_improvement ? 1 : 0,
                result.tx, result.ty, result.tz,
                result.rx, result.ry, result.rz));
            callback->AddPendingEvent(event);
        }

        return result;
    }
};

class RegistrationReviewFrame : public wxFrame {
public:
    RegistrationReviewFrame(
        wxWindow* parent,
        const wxString& ref_dir,
        const wxString& online_dir,
                const wxString& ini_path,
        const RegistrationThread::Result& result)
        : wxFrame(parent, wxID_ANY, wxT("Registration Review"), wxDefaultPosition, wxSize(1320, 920)),
          m_ref_dir(ref_dir),
          m_online_dir(online_dir),
                    m_ini_path(ini_path),
          m_result(result) {
                m_original_tx = m_result.tx;
                m_original_ty = m_result.ty;
                m_original_tz = m_result.tz;
                m_original_rx = m_result.rx;
                m_original_ry = m_result.ry;
                m_original_rz = m_result.rz;
                m_algorithm_transform = m_result.transform;

        SetMinSize(wxSize(1160, 820));

        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

        wxStaticText* title = new wxStaticText(panel, wxID_ANY, wxT("Registration Review"));
        wxFont title_font(16, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        root->Add(title, 0, wxALL, 12);

        wxFlexGridSizer* body = new wxFlexGridSizer(2, 2, 12, 12);
        body->AddGrowableCol(0, 1);
        body->AddGrowableCol(1, 1);
        body->AddGrowableRow(0, 1);
        body->AddGrowableRow(1, 1);

        CreateSliceColumn(panel, body, wxT("Sagittal"), 0, m_sagittal_bitmap, m_sagittal_slider, m_sagittal_label);
        CreateSliceColumn(panel, body, wxT("Coronal"), 1, m_coronal_bitmap, m_coronal_slider, m_coronal_label);
        CreateSliceColumn(panel, body, wxT("Axial"), 2, m_axial_bitmap, m_axial_slider, m_axial_label);

        wxPanel* info_panel = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        wxBoxSizer* info_root = new wxBoxSizer(wxVERTICAL);

        wxArrayString view_choices;
        view_choices.Add(wxT("Blend"));
        view_choices.Add(wxT("Reference"));
        view_choices.Add(wxT("Online"));
        m_view_mode = new wxChoice(info_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, view_choices);
        m_view_mode->SetSelection(0);
        m_view_mode->Bind(wxEVT_CHOICE, &RegistrationReviewFrame::OnViewModeChanged, this);

        const int kControlSliderWidth = 120;

        wxBoxSizer* mode_row = new wxBoxSizer(wxHORIZONTAL);
        mode_row->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Display")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        mode_row->Add(m_view_mode, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        wxArrayString algo_choices;
        algo_choices.Add(wxT("Seed"));
        algo_choices.Add(wxT("Bone"));
        algo_choices.Add(wxT("Grey value"));
        m_review_algorithm_choice = new wxChoice(info_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, algo_choices);
        const int review_algo_index = std::max(0, algo_choices.Index(m_result.algorithm_label));
        m_review_algorithm_choice->SetSelection(review_algo_index);
        m_review_algorithm_choice->Enable(false);

        mode_row->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Algorithm")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
        mode_row->Add(m_review_algorithm_choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        info_root->Add(mode_row, 0, wxALL | wxEXPAND, 10);

        wxBoxSizer* sliders_row = new wxBoxSizer(wxHORIZONTAL);

        wxBoxSizer* level_col = new wxBoxSizer(wxVERTICAL);
        m_level_text = new wxStaticText(info_panel, wxID_ANY, wxT("Level: 0"));
        m_level_slider = new wxSlider(info_panel, wxID_ANY, 0, 0, 4000, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_level_slider->SetMinSize(wxSize(kControlSliderWidth, -1));
        m_level_slider->Bind(wxEVT_SLIDER, &RegistrationReviewFrame::OnDisplayControlChanged, this);
        level_col->Add(m_level_text, 0, wxBOTTOM, 4);
        level_col->Add(m_level_slider, 0, wxEXPAND);
        sliders_row->Add(level_col, 1, wxRIGHT | wxEXPAND, 8);

        wxBoxSizer* window_col = new wxBoxSizer(wxVERTICAL);
        m_window_text = new wxStaticText(info_panel, wxID_ANY, wxT("Window: 0"));
        m_window_slider = new wxSlider(info_panel, wxID_ANY, 400, 1, 4000, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_window_slider->SetMinSize(wxSize(kControlSliderWidth, -1));
        m_window_slider->Bind(wxEVT_SLIDER, &RegistrationReviewFrame::OnDisplayControlChanged, this);
        window_col->Add(m_window_text, 0, wxBOTTOM, 4);
        window_col->Add(m_window_slider, 0, wxEXPAND);
        sliders_row->Add(window_col, 1, wxRIGHT | wxEXPAND, 8);

        wxBoxSizer* blend_col = new wxBoxSizer(wxVERTICAL);
        m_blend_text = new wxStaticText(info_panel, wxID_ANY, wxT("Blend: 50/50"));
        m_blend_slider = new wxSlider(info_panel, wxID_ANY, 50, 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_blend_slider->SetMinSize(wxSize(kControlSliderWidth, -1));
        m_blend_slider->Bind(wxEVT_SLIDER, &RegistrationReviewFrame::OnDisplayControlChanged, this);
        blend_col->Add(m_blend_text, 0, wxBOTTOM, 4);
        blend_col->Add(m_blend_slider, 0, wxEXPAND);
        sliders_row->Add(blend_col, 1, wxEXPAND);
        info_root->Add(sliders_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        wxBoxSizer* action_row = new wxBoxSizer(wxHORIZONTAL);
        m_reset_view_btn = new wxButton(info_panel, wxID_ANY, wxT("Reset View"));
        m_reset_view_btn->Bind(wxEVT_BUTTON, &RegistrationReviewFrame::OnResetView, this);
        action_row->Add(m_reset_view_btn, 0, wxRIGHT, 8);

        m_restore_original_btn = new wxButton(info_panel, wxID_ANY, wxT("Back to auto registrtion"));
        m_restore_original_btn->Bind(wxEVT_BUTTON, &RegistrationReviewFrame::OnRestoreOriginalTransform, this);
        m_restore_original_btn->Enable(false);
        action_row->Add(m_restore_original_btn, 0);
        info_root->Add(action_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        m_status_text = new wxStaticText(info_panel, wxID_ANY, wxT("Status: Ready"));
        info_root->Add(m_status_text, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        wxStaticText* transform_title = new wxStaticText(info_panel, wxID_ANY, wxT("Registration Result (editable)"));
        wxFont transform_font = transform_title->GetFont();
        transform_font.SetWeight(wxFONTWEIGHT_BOLD);
        transform_title->SetFont(transform_font);
        info_root->Add(transform_title, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        wxGridSizer* t_grid = new wxGridSizer(2, 6, 6, 6);
        t_grid->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Tx(mm)")), 0, wxALIGN_CENTER_VERTICAL);
        t_grid->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Ty(mm)")), 0, wxALIGN_CENTER_VERTICAL);
        t_grid->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Tz(mm)")), 0, wxALIGN_CENTER_VERTICAL);
        t_grid->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Rx(deg)")), 0, wxALIGN_CENTER_VERTICAL);
        t_grid->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Ry(deg)")), 0, wxALIGN_CENTER_VERTICAL);
        t_grid->Add(new wxStaticText(info_panel, wxID_ANY, wxT("Rz(deg)")), 0, wxALIGN_CENTER_VERTICAL);

        m_tx_ctrl = new wxSpinCtrlDouble(info_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS);
        m_ty_ctrl = new wxSpinCtrlDouble(info_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS);
        m_tz_ctrl = new wxSpinCtrlDouble(info_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS);
        m_rx_ctrl = new wxSpinCtrlDouble(info_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS);
        m_ry_ctrl = new wxSpinCtrlDouble(info_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS);
        m_rz_ctrl = new wxSpinCtrlDouble(info_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS);

        auto init_spin = [](wxSpinCtrlDouble* spin, double min_v, double max_v, double inc, double value) {
            spin->SetRange(min_v, max_v);
            spin->SetIncrement(inc);
            spin->SetDigits(2);
            spin->SetValue(value);
        };
        init_spin(m_tx_ctrl, -300.0, 300.0, 1.00, m_result.tx);
        init_spin(m_ty_ctrl, -300.0, 300.0, 1.00, m_result.ty);
        init_spin(m_tz_ctrl, -300.0, 300.0, 1.00, m_result.tz);
        init_spin(m_rx_ctrl, -180.0, 180.0, 0.30, m_result.rx);
        init_spin(m_ry_ctrl, -180.0, 180.0, 0.30, m_result.ry);
        init_spin(m_rz_ctrl, -180.0, 180.0, 0.30, m_result.rz);

        t_grid->Add(m_tx_ctrl, 0, wxEXPAND);
        t_grid->Add(m_ty_ctrl, 0, wxEXPAND);
        t_grid->Add(m_tz_ctrl, 0, wxEXPAND);
        t_grid->Add(m_rx_ctrl, 0, wxEXPAND);
        t_grid->Add(m_ry_ctrl, 0, wxEXPAND);
        t_grid->Add(m_rz_ctrl, 0, wxEXPAND);
        info_root->Add(t_grid, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        m_tx_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &RegistrationReviewFrame::OnTransformEditorChangedSpin, this);
        m_ty_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &RegistrationReviewFrame::OnTransformEditorChangedSpin, this);
        m_tz_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &RegistrationReviewFrame::OnTransformEditorChangedSpin, this);
        m_rx_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &RegistrationReviewFrame::OnTransformEditorChangedSpin, this);
        m_ry_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &RegistrationReviewFrame::OnTransformEditorChangedSpin, this);
        m_rz_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &RegistrationReviewFrame::OnTransformEditorChangedSpin, this);
        m_tx_ctrl->Bind(wxEVT_TEXT, &RegistrationReviewFrame::OnTransformEditorChangedText, this);
        m_ty_ctrl->Bind(wxEVT_TEXT, &RegistrationReviewFrame::OnTransformEditorChangedText, this);
        m_tz_ctrl->Bind(wxEVT_TEXT, &RegistrationReviewFrame::OnTransformEditorChangedText, this);
        m_rx_ctrl->Bind(wxEVT_TEXT, &RegistrationReviewFrame::OnTransformEditorChangedText, this);
        m_ry_ctrl->Bind(wxEVT_TEXT, &RegistrationReviewFrame::OnTransformEditorChangedText, this);
        m_rz_ctrl->Bind(wxEVT_TEXT, &RegistrationReviewFrame::OnTransformEditorChangedText, this);

        info_root->AddStretchSpacer();
        info_root->Add(new wxStaticLine(info_panel, wxID_ANY), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        m_help_text = new wxStaticText(info_panel, wxID_ANY, wxT("Guide: Wheel=Zoom  Right Drag=Pan  PgUp/PgDn=Depth Nudge"));
        m_help_text_raw = wxT("Guide: Wheel=Zoom  Right Drag=Pan  PgUp/PgDn=Depth Nudge");
        info_root->Add(m_help_text, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        info_panel->SetSizer(info_root);
        info_panel->SetMinSize(wxSize(480, -1));
        info_panel->SetMaxSize(wxSize(630, -1));
        m_info_panel = info_panel;
        m_info_panel->Bind(wxEVT_SIZE, &RegistrationReviewFrame::OnInfoPanelSize, this);

        wxBoxSizer* info_cell = new wxBoxSizer(wxHORIZONTAL);
        info_cell->AddStretchSpacer();
        info_cell->Add(info_panel, 0, wxEXPAND);
        info_cell->AddStretchSpacer();
        body->Add(info_cell, 1, wxEXPAND);

        root->Add(body, 1, wxALL | wxEXPAND, 12);

        panel->SetSizer(root);
        Bind(wxEVT_CHAR_HOOK, &RegistrationReviewFrame::OnCharHook, this);

        Bind(wxEVT_VOLUMES_LOADED, &RegistrationReviewFrame::OnVolumesLoaded, this);

        UpdateHelpTextWrap();

        Centre();

        // Perform initial layout to establish proper client size before wrapping text
        Layout();
        UpdateHelpTextWrap();

        // Start async DICOM loading so the window can appear immediately
        m_status_text->SetLabel(wxT("Status: Loading volumes, please wait..."));
        StartAsyncLoad();
    }

private:
    void CreateSliceColumn(
        wxWindow* parent,
        wxSizer* root,
        const wxString& title,
        int orientation,
        wxStaticBitmap*& bitmap,
        wxSlider*& slider,
        wxStaticText*& label) {
        wxBoxSizer* column = new wxBoxSizer(wxVERTICAL);
        column->Add(new wxStaticText(parent, wxID_ANY, title), 0, wxBOTTOM, 6);

        bitmap = new wxStaticBitmap(parent, wxID_ANY, wxBitmap(360, 320));
        bitmap->Bind(wxEVT_LEFT_DOWN, [this, orientation](wxMouseEvent& event) {
            OnSliceLeftDown(orientation, event);
        });
        bitmap->Bind(wxEVT_LEFT_DCLICK, [this, orientation](wxMouseEvent& event) {
            OnSliceLeftDClick(orientation, event);
        });
        bitmap->Bind(wxEVT_LEFT_UP, [this, orientation](wxMouseEvent& event) {
            OnSliceLeftUp(orientation, event);
        });
        bitmap->Bind(wxEVT_RIGHT_DOWN, [this, orientation](wxMouseEvent& event) {
            OnSliceRightDown(orientation, event);
        });
        bitmap->Bind(wxEVT_RIGHT_UP, [this, orientation](wxMouseEvent& event) {
            OnSliceRightUp(orientation, event);
        });
        bitmap->Bind(wxEVT_MOTION, [this, orientation](wxMouseEvent& event) {
            OnSliceMotion(orientation, event);
        });
        bitmap->Bind(wxEVT_LEAVE_WINDOW, [this, orientation](wxMouseEvent& event) {
            OnSliceLeaveWindow(orientation, event);
        });
        bitmap->Bind(wxEVT_MOUSEWHEEL, [this, orientation](wxMouseEvent& event) {
            OnSliceMouseWheel(orientation, event);
        });
        column->Add(bitmap, 1, wxEXPAND | wxBOTTOM, 6);

        label = new wxStaticText(parent, wxID_ANY, title);
        column->Add(label, 0, wxBOTTOM, 4);

        slider = new wxSlider(parent, wxID_ANY, 0, 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        slider->Bind(wxEVT_SLIDER, [this, orientation](wxCommandEvent&) {
            UpdateSliceView(orientation);
        });
        column->Add(slider, 0, wxEXPAND);

        root->Add(column, 1, wxEXPAND);
    }

    ~RegistrationReviewFrame() {
        if (m_load_thread && m_load_thread->joinable()) {
            m_load_thread->join();
        }
    }

    void StartAsyncLoad() {
        m_load_thread = std::make_unique<std::thread>([this]() {
            wxString error;
            bool ok = LoadVolumes(error);
            wxCommandEvent evt(wxEVT_VOLUMES_LOADED);
            evt.SetString(ok ? wxString() : error);
            wxPostEvent(this, evt);
        });
    }

    void OnVolumesLoaded(wxCommandEvent& event) {
        const wxString error = event.GetString();
        if (!error.IsEmpty()) {
            wxMessageBox(error, wxT("Registration Review Error"), wxOK | wxICON_ERROR, this);
            Destroy();
            return;
        }
        InitializeSliders();
        UpdateAllViews();
        m_status_text->SetLabel(wxT("Status: Volumes loaded. Ready for review."));
    }

    bool ComputeAutoWindowLevel(double& out_level, double& out_window) const {
        std::vector<double> samples;
        samples.reserve(240000);

        auto collect = [&samples](vtkImageData* image, const int dims[3]) {
            if (!image) {
                return;
            }
            const double total = static_cast<double>(std::max(1, dims[0]) * std::max(1, dims[1]) * std::max(1, dims[2]));
            const double target = 120000.0;
            const int step = std::max(1, static_cast<int>(std::round(std::cbrt(total / target))));
            for (int z = 0; z < dims[2]; z += step) {
                for (int y = 0; y < dims[1]; y += step) {
                    for (int x = 0; x < dims[0]; x += step) {
                        samples.push_back(image->GetScalarComponentAsDouble(x, y, z, 0));
                    }
                }
            }
        };

        collect(m_ref_volume, m_ref_dims);
        collect(m_online_volume, m_online_dims);
        if (samples.size() < 32) {
            return false;
        }

        const size_t low_index = (samples.size() * 2) / 100;
        const size_t high_index = (samples.size() * 98) / 100;
        std::nth_element(samples.begin(), samples.begin() + low_index, samples.end());
        const double low = samples[low_index];
        std::nth_element(samples.begin(), samples.begin() + high_index, samples.end());
        const double high = samples[high_index];
        if (high <= low) {
            return false;
        }

        out_level = 0.5 * (low + high);
        out_window = std::max(80.0, high - low);
        return true;
    }

    int MapRefSliceToOnline(int orientation, int slice) const {
        const int axis = orientation == 0 ? 0 : (orientation == 1 ? 1 : 2);
        const int ref_max = std::max(0, m_ref_dims[axis] - 1);
        const int online_max = std::max(0, m_online_dims[axis] - 1);
        if (ref_max <= 0 || online_max <= 0) {
            return std::clamp(slice, 0, online_max);
        }
        const double t = static_cast<double>(std::clamp(slice, 0, ref_max)) / static_cast<double>(ref_max);
        return std::clamp(static_cast<int>(std::lround(t * online_max)), 0, online_max);
    }

    bool IsOnlineModeSelected() const {
        return m_view_mode && m_view_mode->GetStringSelection() == wxT("Online");
    }

    void OnInfoPanelSize(wxSizeEvent& event) {
        event.Skip();
        // Delay wrap update to allow layout to complete
        wxTheApp->CallAfter([this]() {
            UpdateHelpTextWrap();
        });
    }

    void UpdateHelpTextWrap() {
        if (!m_info_panel) {
            return;
        }
        const int client_width = m_info_panel->GetClientSize().GetWidth();
        const int wrap_width = std::max(450, client_width + (client_width / 2) - 20);
        if (m_help_text) {
            m_help_text->SetLabel(m_help_text_raw);
            m_help_text->Wrap(wrap_width);
        }
        if (m_info_panel) {
            m_info_panel->Layout();
        }
    }

    bool LoadVolumes(wxString& error) {
        if (!LoadDicomVolume(m_ref_dir, m_ref_volume, error)) {
            error = wxT("Failed to load reference DICOM for review: ") + error;
            return false;
        }
        if (!LoadDicomVolume(m_online_dir, m_online_volume, error)) {
            error = wxT("Failed to load online DICOM for review: ") + error;
            return false;
        }

        m_ref_volume->GetDimensions(m_ref_dims);
        m_online_volume->GetDimensions(m_online_dims);
        m_ref_volume->GetScalarRange(m_ref_range);
        m_online_volume->GetScalarRange(m_online_range);
        // 注意: Registration::run返回的A2在本工程中用于ref-grid采样online-grid，直接使用正向矩阵
        m_inverse_transform = m_result.transform;
        m_has_inverse_transform = true;

        double auto_level = 0.0;
        double auto_window = 0.0;
        if (ComputeAutoWindowLevel(auto_level, auto_window)) {
            m_default_level = auto_level;
            m_default_window = auto_window;
        } else {
            const double global_min = std::min(m_ref_range[0], m_online_range[0]);
            const double global_max = std::max(m_ref_range[1], m_online_range[1]);
            m_default_level = 0.5 * (global_min + global_max);
            m_default_window = std::max(1.0, global_max - global_min);
        }
        return true;
    }

    void InitializeSliders() {
        m_sagittal_slider->SetRange(0, std::max(0, m_ref_dims[0] - 1));
        m_sagittal_slider->SetValue(std::max(0, m_ref_dims[0] / 2));

        m_coronal_slider->SetRange(0, std::max(0, m_ref_dims[1] - 1));
        m_coronal_slider->SetValue(std::max(0, m_ref_dims[1] / 2));

        m_axial_slider->SetRange(0, std::max(0, m_ref_dims[2] - 1));
        m_axial_slider->SetValue(std::max(0, m_ref_dims[2] / 2));

        if (m_level_slider) {
            m_level_slider->SetValue(static_cast<int>(std::clamp(m_default_level, 0.0, 4000.0)));
        }
        if (m_window_slider) {
            m_window_slider->SetValue(static_cast<int>(std::clamp(m_default_window, 1.0, 4000.0)));
        }
        if (m_blend_slider) {
            m_blend_slider->SetValue(50);
        }
        for (auto& view : m_views) {
            view.zoom = 1.0;
            view.pan_x = 0.0;
            view.pan_y = 0.0;
            view.is_panning = false;
            view.is_adjusting_wl = false;
        }
        UpdateControlLabels();
    }

    void OnViewModeChanged(wxCommandEvent&) {
        UpdateAllViews();
    }

    void OnDisplayControlChanged(wxCommandEvent&) {
        UpdateControlLabels();
        UpdateAllViews();
    }

    void UpdateControlLabels() {
        if (m_level_text && m_level_slider) {
            m_level_text->SetLabel(wxString::Format(wxT("Level: %d"), m_level_slider->GetValue()));
        }
        if (m_window_text && m_window_slider) {
            m_window_text->SetLabel(wxString::Format(wxT("Window: %d"), m_window_slider->GetValue()));
        }
        if (m_blend_text && m_blend_slider) {
            const int online_weight = m_blend_slider->GetValue();
            m_blend_text->SetLabel(wxString::Format(wxT("Blend: %d/%d"), 100 - online_weight, online_weight));
        }
    }

    void UpdateAllViews() {
        UpdateSliceView(0);
        UpdateSliceView(1);
        UpdateSliceView(2);
        Layout();
    }

    bool IsAtAutoTransformValues(double tx, double ty, double tz, double rx, double ry, double rz) const {
        return std::fabs(tx - m_original_tx) < 1e-6
            && std::fabs(ty - m_original_ty) < 1e-6
            && std::fabs(tz - m_original_tz) < 1e-6
            && std::fabs(rx - m_original_rx) < 1e-6
            && std::fabs(ry - m_original_ry) < 1e-6
            && std::fabs(rz - m_original_rz) < 1e-6;
    }

    registration::Transform BuildManualTransformAroundRefCenter(double tx, double ty, double tz, double rx, double ry, double rz) const {
        registration::Transform fine;
        fine.make_translation_after_rotation(
            static_cast<float>(tx),
            static_cast<float>(ty),
            static_cast<float>(tz),
            static_cast<float>(rx),
            static_cast<float>(ry),
            static_cast<float>(rz));

        float cx = 0.0f;
        float cy = 0.0f;
        float cz = 0.0f;
        if (m_ref_dims[0] > 0 && m_ref_dims[1] > 0 && m_ref_dims[2] > 0) {
            // Keep consistency with REGISTRATION registration default pivot (0.5 * size).
            cx = 0.5f * static_cast<float>(m_ref_dims[0]);
            cy = 0.5f * static_cast<float>(m_ref_dims[1]);
            cz = 0.5f * static_cast<float>(m_ref_dims[2]);
        }

        registration::Transform pm;
        pm.make_translation(cx, cy, cz);
        registration::Transform pm_inv = pm;
        pm_inv.invert();

        registration::Transform out = pm;
        out.post_multiply(fine);
        out.post_multiply(pm_inv);
        return out;
    }

    void ApplyTransformFromEditors() {
        if (m_updating_transform_editors) {
            return;
        }
        if (!m_tx_ctrl || !m_ty_ctrl || !m_tz_ctrl || !m_rx_ctrl || !m_ry_ctrl || !m_rz_ctrl) {
            return;
        }

        const double tx = m_tx_ctrl->GetValue();
        const double ty = m_ty_ctrl->GetValue();
        const double tz = m_tz_ctrl->GetValue();
        const double rx = m_rx_ctrl->GetValue();
        const double ry = m_ry_ctrl->GetValue();
        const double rz = m_rz_ctrl->GetValue();

        m_result.tx = static_cast<float>(tx);
        m_result.ty = static_cast<float>(ty);
        m_result.tz = static_cast<float>(tz);
        m_result.rx = rx;
        m_result.ry = ry;
        m_result.rz = rz;

        if (IsAtAutoTransformValues(tx, ty, tz, rx, ry, rz)) {
            // Exactly restore auto-registration transform and state.
            m_result.transform = m_algorithm_transform;
            m_inverse_transform = m_algorithm_transform;
            m_is_manual_transform = false;
            if (m_restore_original_btn) {
                m_restore_original_btn->Enable(false);
            }
            if (m_status_text) {
                m_status_text->SetLabel(wxT("Status: Using auto registration result."));
            }
        } else {
            // Manual adjust mode: use same rotation-pivot semantics as REGISTRATION registration.
            m_result.transform = BuildManualTransformAroundRefCenter(tx, ty, tz, rx, ry, rz);
            m_inverse_transform = m_result.transform;
            m_is_manual_transform = true;
            if (m_restore_original_btn) {
                m_restore_original_btn->Enable(true);
            }
            if (m_status_text) {
                m_status_text->SetLabel(wxString::Format(
                    wxT("Status: Manual transform T=(%.2f, %.2f, %.2f), R=(%.2f, %.2f, %.2f)"),
                    tx, ty, tz, rx, ry, rz));
            }
        }

        m_has_inverse_transform = true;
        UpdateAllViews();
    }

    void OnTransformEditorChangedSpin(wxSpinDoubleEvent&) {
        ApplyTransformFromEditors();
    }

    void OnTransformEditorChangedText(wxCommandEvent&) {
        ApplyTransformFromEditors();
    }

    void OnRestoreOriginalTransform(wxCommandEvent&) {
        if (!m_tx_ctrl || !m_ty_ctrl || !m_tz_ctrl || !m_rx_ctrl || !m_ry_ctrl || !m_rz_ctrl) {
            return;
        }

        m_updating_transform_editors = true;
        m_tx_ctrl->SetValue(m_original_tx);
        m_ty_ctrl->SetValue(m_original_ty);
        m_tz_ctrl->SetValue(m_original_tz);
        m_rx_ctrl->SetValue(m_original_rx);
        m_ry_ctrl->SetValue(m_original_ry);
        m_rz_ctrl->SetValue(m_original_rz);
        m_updating_transform_editors = false;

        m_result.tx = static_cast<float>(m_original_tx);
        m_result.ty = static_cast<float>(m_original_ty);
        m_result.tz = static_cast<float>(m_original_tz);
        m_result.rx = m_original_rx;
        m_result.ry = m_original_ry;
        m_result.rz = m_original_rz;
        m_result.transform = m_algorithm_transform;
        m_inverse_transform = m_algorithm_transform;
        m_has_inverse_transform = true;
        m_is_manual_transform = false;
        if (m_restore_original_btn) {
            m_restore_original_btn->Enable(false);
        }
        if (m_status_text) {
            m_status_text->SetLabel(wxT("Status: Back to auto registration result."));
        }
        UpdateAllViews();
    }

    void OnResetView(wxCommandEvent&) {
        for (auto& view : m_views) {
            view.zoom = 1.0;
            view.pan_x = 0.0;
            view.pan_y = 0.0;
            view.is_panning = false;
            view.is_adjusting_wl = false;
        }
        if (m_level_slider) {
            m_level_slider->SetValue(static_cast<int>(std::clamp(m_default_level, 0.0, 4000.0)));
        }
        if (m_window_slider) {
            m_window_slider->SetValue(static_cast<int>(std::clamp(m_default_window, 1.0, 4000.0)));
        }
        if (m_blend_slider) {
            m_blend_slider->SetValue(50);
        }
        UpdateControlLabels();
        if (m_status_text) {
            m_status_text->SetLabel(wxT("Status: View and display controls reset."));
        }
        UpdateAllViews();
    }

    void OnCharHook(wxKeyEvent& event) {
        const int key = event.GetKeyCode();
        if (key == 'R' || key == 'r') {
            wxCommandEvent dummy;
            OnResetView(dummy);
            return;
        }
        event.Skip();
    }

    void UpdateSliceView(int orientation) {
        wxSlider* slider = nullptr;
        wxStaticBitmap* bitmap = nullptr;
        wxStaticText* label = nullptr;
        int axis_max = 0;
        const wxString name = orientation == 0 ? wxT("Sagittal") : (orientation == 1 ? wxT("Coronal") : wxT("Axial"));

        if (orientation == 0) {
            slider = m_sagittal_slider;
            bitmap = m_sagittal_bitmap;
            label = m_sagittal_label;
            axis_max = m_ref_dims[0] - 1;
        } else if (orientation == 1) {
            slider = m_coronal_slider;
            bitmap = m_coronal_bitmap;
            label = m_coronal_label;
            axis_max = m_ref_dims[1] - 1;
        } else {
            slider = m_axial_slider;
            bitmap = m_axial_bitmap;
            label = m_axial_label;
            axis_max = m_ref_dims[2] - 1;
        }

        if (!slider || !bitmap || !label || !m_ref_volume || !m_online_volume) {
            return;
        }

        const int slice = ClampIndex(slider->GetValue(), axis_max);
        bitmap->SetBitmap(CreateSliceBitmap(orientation, slice));
        label->SetLabel(wxString::Format(wxT("%s Slice: %d / %d"), name, slice + 1, axis_max + 1));
    }

    void OnSliceLeftDown(int orientation, wxMouseEvent& event) {
        if (event.ShiftDown()) {
            ViewState& view = m_views[orientation];
            view.is_adjusting_wl = true;
            view.last_mouse = event.GetPosition();
            if (m_status_text) {
                m_status_text->SetLabel(wxT("Shift+LeftDrag: adjust window/level"));
            }
            return;
        }

        OnSliceClicked(orientation, event);
    }

    void OnSliceLeftDClick(int, wxMouseEvent&) {
    }

    void OnSliceLeftUp(int orientation, wxMouseEvent& event) {
        (void)event;
        ViewState& view = m_views[orientation];
        if (view.is_adjusting_wl) {
            view.is_adjusting_wl = false;
            if (m_status_text) {
                m_status_text->SetLabel(wxT("Status: Ready"));
            }
        }
        UpdateSliceView(orientation);
    }

    void OnSliceRightDown(int orientation, wxMouseEvent& event) {
        ViewState& view = m_views[orientation];
        view.is_panning = true;
        view.last_mouse = event.GetPosition();
    }

    void OnSliceRightUp(int orientation, wxMouseEvent&) {
        m_views[orientation].is_panning = false;
    }

    void OnSliceMotion(int orientation, wxMouseEvent& event) {
        ViewState& view = m_views[orientation];
        if (view.is_adjusting_wl && event.LeftIsDown()) {
            const wxPoint current = event.GetPosition();
            const wxPoint delta = current - view.last_mouse;
            view.last_mouse = current;
            if (m_level_slider) {
                m_level_slider->SetValue(std::clamp(m_level_slider->GetValue() + delta.y * -4, m_level_slider->GetMin(), m_level_slider->GetMax()));
            }
            if (m_window_slider) {
                m_window_slider->SetValue(std::clamp(m_window_slider->GetValue() + delta.x * 4, m_window_slider->GetMin(), m_window_slider->GetMax()));
            }
            UpdateControlLabels();
            UpdateAllViews();
            return;
        }

        if (view.is_panning && event.RightIsDown()) {
            const wxPoint current = event.GetPosition();
            const wxPoint delta = current - view.last_mouse;
            view.last_mouse = current;

            int src_w = 1;
            int src_h = 1;
            GetPlaneDimensions(orientation, src_w, src_h);
            view.pan_x -= (static_cast<double>(delta.x) / 360.0) * (src_w / view.zoom);
            view.pan_y += (static_cast<double>(delta.y) / 320.0) * (src_h / view.zoom);
            UpdateSliceView(orientation);
            return;
        }

    }

    void OnSliceLeaveWindow(int orientation, wxMouseEvent& event) {
        ApplySliceCursor(orientation, wxCURSOR_ARROW);
        event.Skip();
    }

    void OnSliceMouseWheel(int orientation, wxMouseEvent& event) {
        ViewState& view = m_views[orientation];
        const int rotation = event.GetWheelRotation();
        if (rotation == 0) {
            return;
        }
        const double factor = rotation > 0 ? 1.15 : 1.0 / 1.15;
        view.zoom = std::clamp(view.zoom * factor, 0.25, 8.0);
        UpdateSliceView(orientation);
    }

    void ApplySliceCursor(int orientation, int cursor_id) {
        wxStaticBitmap* bitmap = GetSliceBitmap(orientation);
        if (!bitmap) {
            return;
        }
        bitmap->SetCursor(wxCursor(static_cast<wxStockCursor>(cursor_id)));
    }

    wxStaticBitmap* GetSliceBitmap(int orientation) const {
        if (orientation == 0) {
            return m_sagittal_bitmap;
        }
        if (orientation == 1) {
            return m_coronal_bitmap;
        }
        return m_axial_bitmap;
    }

    void OnSliceClicked(int orientation, wxMouseEvent& event) {
        wxStaticBitmap* bitmap = orientation == 0 ? m_sagittal_bitmap : (orientation == 1 ? m_coronal_bitmap : m_axial_bitmap);
        if (!bitmap) {
            return;
        }

        const wxSize size = bitmap->GetSize();
        if (size.GetWidth() <= 1 || size.GetHeight() <= 1) {
            return;
        }

        const wxPoint pos = event.GetPosition();
        double src_u = 0.0;
        double src_v = 0.0;
        MapDisplayToSource(orientation, pos, src_u, src_v);

        if (orientation == 0) {
            const int ref_y = ClampIndex(static_cast<int>(std::lround(src_u)), m_ref_dims[1] - 1);
            const int ref_z = ClampIndex(static_cast<int>(std::lround(src_v)), m_ref_dims[2] - 1);
            m_coronal_slider->SetValue(ref_y);
            m_axial_slider->SetValue(ref_z);
        } else if (orientation == 1) {
            const int ref_x = ClampIndex(static_cast<int>(std::lround(src_u)), m_ref_dims[0] - 1);
            const int ref_z = ClampIndex(static_cast<int>(std::lround(src_v)), m_ref_dims[2] - 1);
            m_sagittal_slider->SetValue(ref_x);
            m_axial_slider->SetValue(ref_z);
        } else {
            const int ref_x = ClampIndex(static_cast<int>(std::lround(src_u)), m_ref_dims[0] - 1);
            const int ref_y = ClampIndex(static_cast<int>(std::lround(src_v)), m_ref_dims[1] - 1);
            m_sagittal_slider->SetValue(ref_x);
            m_coronal_slider->SetValue(ref_y);
        }

        UpdateAllViews();
    }

    void GetDisplayRange(double range[2]) const {
        const double level = m_level_slider ? static_cast<double>(m_level_slider->GetValue()) : m_default_level;
        const double window = m_window_slider ? std::max(1.0, static_cast<double>(m_window_slider->GetValue())) : m_default_window;
        range[0] = level - 0.5 * window;
        range[1] = level + 0.5 * window;
    }

    void GetPlaneDimensions(int orientation, int& src_width, int& src_height) const {
        const bool online_mode = IsOnlineModeSelected();
        const int* dims = online_mode ? m_online_dims : m_ref_dims;
        if (orientation == 0) {
            src_width = std::max(1, dims[1]);
            src_height = std::max(1, dims[2]);
        } else if (orientation == 1) {
            src_width = std::max(1, dims[0]);
            src_height = std::max(1, dims[2]);
        } else {
            src_width = std::max(1, dims[0]);
            src_height = std::max(1, dims[1]);
        }
    }

    void MapDisplayToSource(int orientation, const wxPoint& point, double& src_u, double& src_v) const {
        int src_width = 1;
        int src_height = 1;
        GetPlaneDimensions(orientation, src_width, src_height);
        const ViewState& view = m_views[orientation];
        const double visible_width = src_width / view.zoom;
        const double visible_height = src_height / view.zoom;
        const double center_u = 0.5 * (src_width - 1) + view.pan_x;
        const double center_v = 0.5 * (src_height - 1) + view.pan_y;
        const double nx = (static_cast<double>(std::max(0, std::min(359, point.x))) + 0.5) / 360.0 - 0.5;
        const double ny = 0.5 - (static_cast<double>(std::max(0, std::min(319, point.y))) + 0.5) / 320.0;
        src_u = center_u + nx * visible_width;
        src_v = center_v + ny * visible_height;
    }

    bool MapSourceToDisplay(int orientation, double src_u, double src_v, int& disp_x, int& disp_y) const {
        int src_width = 1;
        int src_height = 1;
        GetPlaneDimensions(orientation, src_width, src_height);
        const ViewState& view = m_views[orientation];
        const double visible_width = src_width / view.zoom;
        const double visible_height = src_height / view.zoom;
        const double center_u = 0.5 * (src_width - 1) + view.pan_x;
        const double center_v = 0.5 * (src_height - 1) + view.pan_y;
        const double nx = (src_u - center_u) / visible_width;
        const double ny = (src_v - center_v) / visible_height;
        disp_x = static_cast<int>(std::lround((nx + 0.5) * 360.0));
        disp_y = static_cast<int>(std::lround((0.5 - ny) * 320.0));
        return disp_x >= 0 && disp_x < 360 && disp_y >= 0 && disp_y < 320;
    }

    void DrawCrosshair(wxImage& image, int orientation) const {
        unsigned char* data = image.GetData();
        if (!data) {
            return;
        }

        const int width = image.GetWidth();
        const int height = image.GetHeight();
        int cross_x = 0;
        int cross_y = 0;

        bool visible = false;
        if (orientation == 0) {
            visible = MapSourceToDisplay(0, m_coronal_slider->GetValue(), m_axial_slider->GetValue(), cross_x, cross_y);
        } else if (orientation == 1) {
            visible = MapSourceToDisplay(1, m_sagittal_slider->GetValue(), m_axial_slider->GetValue(), cross_x, cross_y);
        } else {
            visible = MapSourceToDisplay(2, m_sagittal_slider->GetValue(), m_coronal_slider->GetValue(), cross_x, cross_y);
        }

        if (!visible) {
            return;
        }

        cross_x = ClampIndex(cross_x, width - 1);
        cross_y = ClampIndex(cross_y, height - 1);

        for (int x = 0; x < width; ++x) {
            const int index = (cross_y * width + x) * 3;
            data[index + 0] = 255;
            data[index + 1] = 64;
            data[index + 2] = 64;
        }
        for (int y = 0; y < height; ++y) {
            const int index = (y * width + cross_x) * 3;
            data[index + 0] = 255;
            data[index + 1] = 64;
            data[index + 2] = 64;
        }
    }

    wxString BuildResultText(bool accepted) const {
        wxString text;
        text += wxString::Format(wxT("Accepted=%d\n"), accepted ? 1 : 0);
        text += wxT("AcceptedAt=") + wxDateTime::Now().FormatISOCombined(' ') + wxT("\n");
        text += wxT("IniPath=") + m_ini_path + wxT("\n");
        text += wxT("ReferenceDir=") + m_ref_dir + wxT("\n");
        text += wxT("OnlineDir=") + m_online_dir + wxT("\n");
        text += wxT("MatchAlgorithm=") + m_result.algorithm_label + wxT("\n");
        text += wxString::Format(wxT("TranslationMm=%.6f,%.6f,%.6f\n"), m_result.tx, m_result.ty, m_result.tz);
        text += wxString::Format(wxT("RotationDeg=%.6f,%.6f,%.6f\n"), m_result.rx, m_result.ry, m_result.rz);
        text += wxString::Format(wxT("EvalCount=%d\n"), m_result.eval_count);
        text += wxString::Format(wxT("StartCost=%.8f\nFinalCost=%.8f\n"), m_result.start_cf, m_result.end_cf);
        text += wxT("StateInfo=") + m_result.info + wxT("\n");
        text += wxT("TransformMatrix=") + wxString::FromUTF8(m_result.transform.as_string()) + wxT("\n");
        for (int i = 0; i < 3; ++i) {
            const ViewState& view = m_views[i];
            const wxString name = i == 0 ? wxT("Sagittal") : (i == 1 ? wxT("Coronal") : wxT("Axial"));
            text += wxString::Format(wxT("%sZoom=%.4f\n%sPan=%.4f,%.4f\n"), name, view.zoom, name, view.pan_x, view.pan_y);
        }
        return text;
    }

    bool SaveResultToPath(const wxString& path, bool accepted) const {
        std::ofstream out(std::string(path.mb_str()), std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << std::string(BuildResultText(accepted).mb_str());
        out.close();
        return true;
    }

    wxBitmap CreateSliceBitmap(int orientation, int slice) const {
        constexpr int width = 360;
        constexpr int height = 320;
        int src_width = 1;
        int src_height = 1;
        GetPlaneDimensions(orientation, src_width, src_height);
        const ViewState& view = m_views[orientation];
        const double visible_width = src_width / view.zoom;
        const double visible_height = src_height / view.zoom;
        const double center_u = 0.5 * (src_width - 1) + view.pan_x;
        const double center_v = 0.5 * (src_height - 1) + view.pan_y;
        wxImage image(width, height);
        unsigned char* data = image.GetData();
        const wxString mode = m_view_mode ? m_view_mode->GetStringSelection() : wxString(wxT("Blend"));
        const bool online_native_mode = mode == wxT("Online");
        const int online_weight = m_blend_slider ? m_blend_slider->GetValue() : 50;
        const int ref_weight = 100 - online_weight;
        const int mapped_online_slice = online_native_mode ? MapRefSliceToOnline(orientation, slice) : slice;
        double display_range[2] = {0.0, 0.0};
        GetDisplayRange(display_range);

        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const double nx = (static_cast<double>(col) + 0.5) / width - 0.5;
                const double ny = 0.5 - (static_cast<double>(row) + 0.5) / height;
                const double src_u = center_u + nx * visible_width;
                const double src_v = center_v + ny * visible_height;

                double ref_x = 0.0;
                double ref_y = 0.0;
                double ref_z = 0.0;
                if (orientation == 0) {
                    ref_x = slice;
                    ref_y = src_u;
                    ref_z = src_v;
                } else if (orientation == 1) {
                    ref_x = src_u;
                    ref_y = slice;
                    ref_z = src_v;
                } else {
                    ref_x = src_u;
                    ref_y = src_v;
                    ref_z = slice;
                }

                const double ref_value = SampleImageTrilinear(m_ref_volume, ref_x, ref_y, ref_z, display_range[0]);
                double online_value = 0.0;
                if (online_native_mode) {
                    double online_x = 0.0;
                    double online_y = 0.0;
                    double online_z = 0.0;
                    if (orientation == 0) {
                        online_x = mapped_online_slice;
                        online_y = src_u;
                        online_z = src_v;
                    } else if (orientation == 1) {
                        online_x = src_u;
                        online_y = mapped_online_slice;
                        online_z = src_v;
                    } else {
                        online_x = src_u;
                        online_y = src_v;
                        online_z = mapped_online_slice;
                    }
                    online_value = SampleImageTrilinear(m_online_volume, online_x, online_y, online_z, display_range[0]);
                } else if (m_has_inverse_transform) {
                    double online_x = 0.0;
                    double online_y = 0.0;
                    double online_z = 0.0;
                    TransformPoint(m_inverse_transform, ref_x, ref_y, ref_z, online_x, online_y, online_z);
                    online_value = SampleImageTrilinear(m_online_volume, online_x, online_y, online_z, display_range[0]);
                } else {
                    online_value = SampleImageTrilinear(m_online_volume, ref_x, ref_y, ref_z, display_range[0]);
                }

                const unsigned char ref_gray = NormalizeToByte(ref_value, display_range);
                const unsigned char online_gray = NormalizeToByte(online_value, display_range);

                unsigned char red = 0;
                unsigned char green = 0;
                unsigned char blue = 0;

                if (mode == wxT("Reference")) {
                    red = ref_gray;
                    green = ref_gray;
                    blue = ref_gray;
                } else if (mode == wxT("Online")) {
                    red = online_gray;
                    green = online_gray;
                    blue = online_gray;
                } else {
                    red = static_cast<unsigned char>((online_gray * online_weight + ref_gray * ref_weight) / 100);
                    green = ref_gray;
                    blue = online_gray;
                }

                const int index = (row * width + col) * 3;
                data[index + 0] = red;
                data[index + 1] = green;
                data[index + 2] = blue;
            }
        }

        DrawCrosshair(image, orientation);
        return wxBitmap(image);
    }

private:
    struct ViewState {
        double zoom = 1.0;
        double pan_x = 0.0;
        double pan_y = 0.0;
        bool is_panning = false;
        bool is_adjusting_wl = false;
        wxPoint last_mouse;
    };

    wxString m_ref_dir;
    wxString m_online_dir;
    wxString m_ini_path;
    RegistrationThread::Result m_result;
    vtkSmartPointer<vtkImageData> m_ref_volume;
    vtkSmartPointer<vtkImageData> m_online_volume;
    int m_ref_dims[3] = {0, 0, 0};
    int m_online_dims[3] = {0, 0, 0};
    double m_ref_range[2] = {0.0, 0.0};
    double m_online_range[2] = {0.0, 0.0};
    registration::Transform m_inverse_transform;
    bool m_has_inverse_transform = false;

    std::unique_ptr<std::thread> m_load_thread;

    wxChoice* m_view_mode = nullptr;
    wxChoice* m_review_algorithm_choice = nullptr;
    wxPanel* m_info_panel = nullptr;
    wxStaticText* m_status_text = nullptr;
    wxStaticText* m_help_text = nullptr;
    wxSpinCtrlDouble* m_tx_ctrl = nullptr;
    wxSpinCtrlDouble* m_ty_ctrl = nullptr;
    wxSpinCtrlDouble* m_tz_ctrl = nullptr;
    wxSpinCtrlDouble* m_rx_ctrl = nullptr;
    wxSpinCtrlDouble* m_ry_ctrl = nullptr;
    wxSpinCtrlDouble* m_rz_ctrl = nullptr;
    wxButton* m_restore_original_btn = nullptr;
    double m_original_tx = 0.0;
    double m_original_ty = 0.0;
    double m_original_tz = 0.0;
    double m_original_rx = 0.0;
    double m_original_ry = 0.0;
    double m_original_rz = 0.0;
    registration::Transform m_algorithm_transform;
    bool m_updating_transform_editors = false;
    bool m_is_manual_transform = false;
    wxString m_help_text_raw;
    wxStaticText* m_level_text = nullptr;
    wxStaticText* m_window_text = nullptr;
    wxStaticText* m_blend_text = nullptr;
    wxStaticBitmap* m_sagittal_bitmap = nullptr;
    wxStaticBitmap* m_coronal_bitmap = nullptr;
    wxStaticBitmap* m_axial_bitmap = nullptr;
    wxSlider* m_level_slider = nullptr;
    wxSlider* m_window_slider = nullptr;
    wxSlider* m_blend_slider = nullptr;
    wxSlider* m_sagittal_slider = nullptr;
    wxSlider* m_coronal_slider = nullptr;
    wxSlider* m_axial_slider = nullptr;
    wxStaticText* m_sagittal_label = nullptr;
    wxStaticText* m_coronal_label = nullptr;
    wxStaticText* m_axial_label = nullptr;
    wxButton* m_reset_view_btn = nullptr;
    double m_default_level = 0.0;
    double m_default_window = 1.0;
    ViewState m_views[3];
};

class RegistrationMainFrame : public wxFrame {
public:
    explicit RegistrationMainFrame(const StartupInfo& info)
        : wxFrame(nullptr, wxID_ANY, wxT("Registration"), wxDefaultPosition, wxSize(920, 680)),
          m_reg_thread(nullptr) {
        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

        wxStaticText* title = new wxStaticText(panel, wxID_ANY, wxT("C++ Registration Console"));
        wxFont title_font(14, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        root->Add(title, 0, wxALL, 12);

        wxString mode_text = wxString::Format(wxT("Startup Mode: %s"), wxString::FromUTF8(info.mode));
        wxStaticText* mode_label = new wxStaticText(panel, wxID_ANY, mode_text);
        root->Add(mode_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        wxBoxSizer* state_row = new wxBoxSizer(wxHORIZONTAL);
        state_row->Add(new wxStaticText(panel, wxID_ANY, wxT("State")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_state_ctrl = new wxTextCtrl(panel, wxID_ANY, wxString::Format(wxT("%d"), info.state));
        m_state_ctrl->SetMinSize(wxSize(80, -1));
        state_row->Add(m_state_ctrl, 0, wxRIGHT, 16);

        wxArrayString match_algo_choices;
        match_algo_choices.Add(wxT("Seed"));
        match_algo_choices.Add(wxT("Bone"));
        match_algo_choices.Add(wxT("Grey value"));
        state_row->Add(new wxStaticText(panel, wxID_ANY, wxT("Match Algorithm")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_match_algorithm_ctrl = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, match_algo_choices);
        m_match_algorithm_ctrl->SetSelection(2);
        state_row->Add(m_match_algorithm_ctrl, 0, wxRIGHT, 16);

        state_row->Add(new wxStaticText(panel, wxID_ANY, wxT("INI Path")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        wxString ini_path;
        if (info.ini_path.empty()) {
            ini_path = DefaultRegistrationIniPath();
        } else {
            ini_path = wxString::FromUTF8(info.ini_path);
            if (!wxFileExists(ini_path)) {
                ini_path = DefaultRegistrationIniPath();
            }
        }
        m_ini_ctrl = new wxTextCtrl(panel, wxID_ANY, ini_path);
        state_row->Add(m_ini_ctrl, 1, wxEXPAND);
        root->Add(state_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        wxBoxSizer* ref_row = new wxBoxSizer(wxHORIZONTAL);
        ref_row->Add(new wxStaticText(panel, wxID_ANY, wxT("Reference DICOM")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_ref_dir_ctrl = new wxTextCtrl(panel, wxID_ANY, DefaultReferenceDicomDir());
        ref_row->Add(m_ref_dir_ctrl, 1, wxRIGHT | wxEXPAND, 8);
        wxButton* ref_browse_btn = new wxButton(panel, wxID_ANY, wxT("Browse..."));
        ref_browse_btn->Bind(wxEVT_BUTTON, &RegistrationMainFrame::OnBrowseRefDir, this);
        ref_row->Add(ref_browse_btn, 0);
        root->Add(ref_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        wxBoxSizer* online_row = new wxBoxSizer(wxHORIZONTAL);
        online_row->Add(new wxStaticText(panel, wxID_ANY, wxT("Online DICOM")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_online_dir_ctrl = new wxTextCtrl(panel, wxID_ANY, DefaultOnlineDicomDir());
        online_row->Add(m_online_dir_ctrl, 1, wxRIGHT | wxEXPAND, 8);
        wxButton* online_browse_btn = new wxButton(panel, wxID_ANY, wxT("Browse..."));
        online_browse_btn->Bind(wxEVT_BUTTON, &RegistrationMainFrame::OnBrowseOnlineDir, this);
        online_row->Add(online_browse_btn, 0);
        root->Add(online_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        wxBoxSizer* button_row = new wxBoxSizer(wxHORIZONTAL);
        m_run_btn = new wxButton(panel, wxID_ANY, wxT("Run Registration"));
        m_run_btn->Bind(wxEVT_BUTTON, &RegistrationMainFrame::OnRunRegistration, this);
        button_row->Add(m_run_btn, 0, wxRIGHT, 8);

        wxButton* close_btn = new wxButton(panel, wxID_CLOSE, wxT("Close"));
        close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(true); });
        button_row->Add(close_btn, 0);
        root->Add(button_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        root->Add(new wxStaticText(panel, wxID_ANY, wxT("Session Log")), 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        m_log_ctrl = new wxTextCtrl(
            panel,
            wxID_ANY,
            wxEmptyString,
            wxDefaultPosition,
            wxSize(860, 380),
            wxTE_MULTILINE | wxTE_READONLY);
        root->Add(m_log_ctrl, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        AppendLog(wxT("[Registration] Registration window started -- Mode: 6-DOF (6 Degrees of Freedom)"));
        AppendLog(wxT("[Registration] 6-DOF registration: 3 translations (TX, TY, TZ in mm) + 3 rotations (RX, RY, RZ in degrees)"));
        for (const auto& line : info.lines) {
            AppendLog(wxString::FromUTF8(line));
        }
        if (info.fastmode) {
            AppendLog(wxT("[Registration] Warning: /fastmode is typically not used for registration."));
        }

        panel->SetSizer(root);
        Centre();

        // 绑定线程事件处理器
        Bind(wxEVT_REG_LOG, &RegistrationMainFrame::OnRegLog, this);
        Bind(wxEVT_REG_ERROR, &RegistrationMainFrame::OnRegError, this);
        Bind(wxEVT_REG_COMPLETE, &RegistrationMainFrame::OnRegComplete, this);
    }

private:
    void AppendLog(const wxString& line) {
        if (m_log_ctrl) {
            m_log_ctrl->AppendText(line + wxT("\n"));
        }
    }

    void BrowseTo(wxTextCtrl* target) {
        if (!target) {
            return;
        }
        wxString initial_dir = target->GetValue();
        initial_dir = ExistingDirectoryOrDicomDataRoot(initial_dir);
        wxDirDialog dialog(this, wxT("Select DICOM directory"), initial_dir, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            target->SetValue(dialog.GetPath());
        }
    }

    void OnBrowseRefDir(wxCommandEvent&) {
        BrowseTo(m_ref_dir_ctrl);
    }

    void OnBrowseOnlineDir(wxCommandEvent&) {
        BrowseTo(m_online_dir_ctrl);
    }

    bool ValidateDicomDir(const wxString& label, const wxString& path, size_t& out_count) {
        if (path.IsEmpty()) {
            AppendLog(wxString::Format(wxT("[Registration] %s path is empty."), label));
            return false;
        }
        if (!wxDirExists(path)) {
            AppendLog(wxString::Format(wxT("[Registration] %s directory not found: %s"), label, path));
            return false;
        }

        out_count = CountDicomFilesInDir(path);
        if (out_count == 0) {
            AppendLog(wxString::Format(wxT("[Registration] %s has no DICOM-like files: %s"), label, path));
            return false;
        }

        AppendLog(wxString::Format(wxT("[Registration] %s DICOM files: %zu"), label, out_count));
        return true;
    }

    void OnRunRegistration(wxCommandEvent&) {
        // 检查是否已有线程运行
        if (m_reg_thread && m_reg_thread->joinable()) {
            AppendLog(wxT("[Registration] Registration already running. Please wait..."));
            return;
        }

        long state = 0;
        if (!m_state_ctrl || !m_state_ctrl->GetValue().ToLong(&state)) {
            AppendLog(wxT("[Registration] Invalid State. Please input an integer (1..6)."));
            return;
        }

        const wxString ini_path = m_ini_ctrl ? m_ini_ctrl->GetValue() : wxString();
        if (ini_path.IsEmpty()) {
            AppendLog(wxT("[Registration] INI path is empty."));
            return;
        }
        if (!wxFileExists(ini_path)) {
            AppendLog(wxString::Format(wxT("[Registration] INI file not found: %s"), ini_path));
            return;
        }

        if (state != 3) {
            AppendLog(wxString::Format(
                wxT("[Registration] Warning: current State=%ld, registration usually uses State=3."), state));
        }

        size_t ref_count = 0;
        size_t online_count = 0;
        if (!ValidateDicomDir(wxT("Reference"), m_ref_dir_ctrl->GetValue(), ref_count)) {
            return;
        }
        if (!ValidateDicomDir(wxT("Online"), m_online_dir_ctrl->GetValue(), online_count)) {
            return;
        }

        // 禁用按钮
        if (m_run_btn) {
            m_run_btn->Enable(false);
            m_run_btn->SetLabel(wxT("Running..."));
        }

        AppendLog(wxT("[Registration] Starting registration in background thread..."));

        // 准备参数
        RegistrationThread::Params params;
        params.ref_dir = m_ref_dir_ctrl->GetValue();
        params.online_dir = m_online_dir_ctrl->GetValue();
        params.algorithm = m_match_algorithm_ctrl
            ? MatchAlgorithmFromLabel(m_match_algorithm_ctrl->GetStringSelection())
            : MatchAlgorithm::kGreyValue;
        m_last_ref_dir = params.ref_dir;
        m_last_online_dir = params.online_dir;

        AppendLog(wxString::Format(
            wxT("[Registration] Selected match algorithm: %s"),
            MatchAlgorithmToLabel(params.algorithm)));

        // 启动后台线程
        m_reg_thread = std::make_unique<std::thread>([this, params]() {
            RegistrationThread::Result result = RegistrationThread::RunRegistration(params, this);
            
            if (result.success) {
                wxCommandEvent event(wxEVT_REG_COMPLETE);
                event.SetClientData(new RegistrationThread::Result(result));
                this->AddPendingEvent(event);
            }
        });
    }

    void OnRegLog(wxCommandEvent& event) {
        const wxString line = event.GetString();
        if (line.StartsWith(wxT("[Registration]"))) {
            AppendLog(line);
        } else {
            AppendLog(wxT("[Registration] ") + line);
        }
    }

    void OnRegError(wxCommandEvent& event) {
        const wxString line = event.GetString();
        if (line.StartsWith(wxT("[Registration]"))) {
            AppendLog(line);
        } else {
            AppendLog(wxT("[Registration] ERROR: ") + line);
        }

        FinalizeRegistrationRun();
    }

    void OnRegComplete(wxCommandEvent& event) {
        auto result = static_cast<RegistrationThread::Result*>(event.GetClientData());
        if (!result) {
            AppendLog(wxT("ERROR: Failed to get registration result."));
            FinalizeRegistrationRun();
            return;
        }

        // 保存结果供后续使用
        float tx = result->tx, ty = result->ty, tz = result->tz;
        double rx = result->rx, ry = result->ry, rz = result->rz;
        int evals = result->eval_count;
        double start_cf = result->start_cf, end_cf = result->end_cf;
        wxString info = result->info;

        AppendLog(wxT("========== REGISTRATION COMPLETED =========="));
        AppendLog(wxT("Transform Result:"));
        AppendLog(wxString::Format(
            wxT("  Translation (mm): Tx=%.3f, Ty=%.3f, Tz=%.3f"),
            tx, ty, tz));
        AppendLog(wxString::Format(
            wxT("  Rotation (deg):   Rx=%.3f, Ry=%.3f, Rz=%.3f"),
            rx, ry, rz));
        AppendLog(wxString::Format(
            wxT("  Match Algorithm:  %s"),
            result->algorithm_label));
        AppendLog(wxT("Optimizer Statistics:"));
        AppendLog(wxString::Format(
            wxT("  Evaluations: %d"), evals));
        AppendLog(wxString::Format(
            wxT("  Initial Cost: %.6f"), start_cf));
        AppendLog(wxString::Format(
            wxT("  Final Cost:   %.6f"), end_cf));
        AppendLog(wxString::Format(
            wxT("  State Info: %s"), info));
        if (m_last_ref_dir.CmpNoCase(m_last_online_dir) == 0) {
            const double max_abs_t = std::max({std::fabs(tx), std::fabs(ty), std::fabs(tz)});
            const double max_abs_r = std::max({std::fabs(rx), std::fabs(ry), std::fabs(rz)});
            AppendLog(wxString::Format(
                wxT("  Same-volume sanity check: |T|max=%.4f mm, |R|max=%.4f deg (expected close to 0)"),
                max_abs_t, max_abs_r));
        }
        AppendLog(wxT("============================================"));

        if (m_review_frame) {
            m_review_frame->Destroy();
            m_review_frame = nullptr;
        }
        m_review_frame = new RegistrationReviewFrame(this, m_last_ref_dir, m_last_online_dir, m_ini_ctrl ? m_ini_ctrl->GetValue() : wxString(), *result);
        m_review_frame->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& close_event) {
            m_review_frame = nullptr;
            close_event.Skip();
        });
        m_review_frame->Show(true);
        m_review_frame->Raise();

        delete result;

        FinalizeRegistrationRun();
    }

    void FinalizeRegistrationRun() {
        if (m_reg_thread && m_reg_thread->joinable()) {
            m_reg_thread->join();
        }
        m_reg_thread.reset();

        if (m_run_btn) {
            m_run_btn->Enable(true);
            m_run_btn->SetLabel(wxT("Run Registration"));
        }
    }

private:
    wxTextCtrl* m_state_ctrl = nullptr;
    wxTextCtrl* m_ini_ctrl = nullptr;
    wxTextCtrl* m_ref_dir_ctrl = nullptr;
    wxTextCtrl* m_online_dir_ctrl = nullptr;
    wxChoice* m_match_algorithm_ctrl = nullptr;
    wxTextCtrl* m_log_ctrl = nullptr;
    wxButton* m_run_btn = nullptr;
    std::unique_ptr<std::thread> m_reg_thread;
    wxString m_last_ref_dir;
    wxString m_last_online_dir;
    RegistrationReviewFrame* m_review_frame = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// Factory function: callable from gui_app.cpp without exposing internals.
// RegistrationMainFrame lives in the anonymous namespace above but is still accessible
// within the same translation unit.
// ---------------------------------------------------------------------------
wxFrame* registration_ui::CreateRegistrationWindow(wxWindow* parent) {
    StartupInfo info;
    RegistrationMainFrame* frame = new RegistrationMainFrame(info);
    if (parent) {
        frame->SetParent(parent);
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Standalone application entry — only compiled when building registration_cpp_app.exe
// ---------------------------------------------------------------------------
#ifdef REGISTRATION_STANDALONE

class RegistrationApp : public wxApp {
public:
    bool OnInit() override {
        StartupInfo info = ParseStartupInfo(argc, argv);
        RegistrationMainFrame* frame = new RegistrationMainFrame(info);
        frame->Show(true);
        SetTopWindow(frame);
        return true;
    }
};

wxIMPLEMENT_APP(RegistrationApp);

#endif  // REGISTRATION_STANDALONE



