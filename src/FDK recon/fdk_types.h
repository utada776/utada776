#pragma once

// ============================================================================
// fdk_types.h
//
// FDK 锥形束 CT 重建共享数据类型定义。
//
// 本文件定义三类核心数据结构：
//   1. ProjectionImage  ── 单帧 2D X 射线投影图像
//   2. Volume3D         ── 三维重建体（float 精度体素）
//   3. FdkParams        ── 重建参数块（全部可配置参数）
//
// 这些结构为 UI、投影加载、RTK 重建后端和导出流程共享。
// ============================================================================

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fdk {

// ============================================================================
// ProjectionImage  ── 单帧投影图像
//
// 存储一张已经过增益/暗场校正并完成 -ln(I/I0) 对数变换的 2D 投影。
// 像素布局：行主序，pixels[row * width + col]。
// gantry_angle_deg 对应采集时机架（X 射线管）的绕等中心旋转角（度）。
// ============================================================================
struct ProjectionImage {
    int width  = 0;            ///< 探测器列数（像素）
    int height = 0;            ///< 探测器行数（像素）
    std::vector<float> pixels; ///< 对数化线积分值，行主序 [row * width + col]

    float gantry_angle_deg = 0.0f; ///< 采集时的机架角度（°）
    int   frame_id         = -1;   ///< 原始帧编号（来自 .FrameIDs 文件）
    float u_centre         = 0.0f; ///< 探测器U列偏移（像素，已缩放至当前分辨率）
    float v_centre         = 0.0f; ///< 探测器V行偏移（像素，已缩放至当前分辨率）
    float detector_pixel_size_mm = 0.0f; ///< 探测器像素间距（mm），用于虚拟探测器保持原始采样间距
    /// 判断图像是否有效（尺寸非零且像素缓冲区与尺寸匹配）
    bool valid() const {
        return width > 0 && height > 0 &&
               static_cast<int>(pixels.size()) == width * height;
    }
};


// ============================================================================
// Volume3D  ── 三维重建体
//
// 体素以各向同性间距 voxel_size_cm 均匀排列在 nx×ny×nz 网格上。
// 坐标系原点位于体中心（等中心）。
// 数据布局：[z * ny * nx + y * nx + x]（Z-Y-X 行主序）。
//
// 重建完成后体素值为经 scale_out / offset_out 缩放的 HU 类似单位：
//   HU ≈ voxel × scale_out + offset_out
// 典型：空气 ≈ -1000 HU，软组织 ≈ 0..+100 HU，骨骼 ≈ +400..+1000 HU。
// ============================================================================
struct Volume3D {
    int nx = 0;    ///< X 方向体素数（左右，通常 256 或 512）
    int ny = 0;    ///< Y 方向体素数（前后）
    int nz = 0;    ///< Z 方向体素数（头足轴）
    std::vector<float> data; ///< 体素数据，行主序 [z*ny*nx + y*nx + x]
    float voxel_size_cm = 0.1f; ///< 各向同性体素尺寸（单位：厘米，1mm = 0.1cm）

    /// 判断体是否已分配且尺寸一致
    bool valid() const {
        return nx > 0 && ny > 0 && nz > 0 &&
               static_cast<int>(data.size()) == nx * ny * nz;
    }

    /// 只读访问指定坐标的体素值
    float  at(int x, int y, int z) const {
        return data[static_cast<std::size_t>(z) * ny * nx +
                    static_cast<std::size_t>(y) * nx + x];
    }
    /// 可写访问指定坐标的体素值
    float& at(int x, int y, int z) {
        return data[static_cast<std::size_t>(z) * ny * nx +
                    static_cast<std::size_t>(y) * nx + x];
    }
};


// ============================================================================
// FrameInfo  ── 单帧角度/编号信息
//
// 从 .FrameIDs 或 .Angles 文件中读取，描述每一帧投影对应的机架角。
// ============================================================================
struct FrameInfo {
    int   frame_id         = -1;   ///< 帧编号（对应磁盘文件名中的序号）
    float gantry_angle_deg = 0.0f; ///< 该帧采集时的机架角（°）
    int   detector_cols    = 512;  ///< 探测器列数（来自XML或默认值）
    int   detector_rows    = 512;  ///< 探测器行数（来自XML或默认值）
    float u_centre         = 0.0f; ///< 探测器U方向中心偏移（1024像素坐标系）〔来自XML UCentre〕
    float v_centre         = 0.0f; ///< 探测器V方向中心偏移（1024像素坐标系）〔来自XML VCentre〕
};


// ============================================================================
// FdkParams  ── 重建参数块
//
// 完整描述一次 FDK 重建所需的全部参数，对应 NKI XVI 中的 `rp` 类。
// 参数分为七组：
//   [1] 重建体几何（尺寸、体素大小、偏移）
//   [2] 扫描几何（FDD、FID、扫描角度范围）
//   [3] 探测器面板（列行数、物理尺寸、准直裁剪）
//   [4] 输入文件路径
//   [5] 滤波与后处理（滤波器类型、Parker 权重、输出缩放）
//   [6] 校正因子（增益、散射、鬼影）
//   [7] 多线程配置
// ============================================================================
struct FdkParams {

    // ── [1] 重建体几何 ─────────────────────────────────────────────────────
    int   nx             = 256;   ///< 重建体 X 方向体素数（建议为 2 的幂次方以加速 FFT）
    int   ny             = 256;   ///< 重建体 Y 方向体素数
    int   nz             = 256;   ///< 重建体 Z 方向体素数（头足轴）
    float voxel_size_cm  = 0.1f;  ///< 各向同性体素尺寸（厘米），典型值 1mm = 0.1cm

    /// 重建体等中心相对几何等中心的偏移（厘米）。
    /// 常用于偏心重建或视野外目标定位。
    float offset_x_cm = 0.0f;
    float offset_y_cm = 0.0f;
    float offset_z_cm = 0.0f;
    float detector_offset_u_scale = 1.0f;
    float detector_offset_v_scale = 1.0f;

    // ── [2] 扫描几何 ───────────────────────────────────────────────────────
    float fdd_cm         = 153.6f; ///< 焦点到探测器距离 FDD（cm）
    float fid_cm         = 100.0f; ///< 焦点到等中心距离 FID（cm），又称 SID/SAD

    float gantry_start_angle_deg = 0.0f;   ///< 首帧机架角（°）
    float gantry_stop_angle_deg  = 360.0f; ///< 末帧机架角（°）
    float cone_angle_deg         = 7.0f;   ///< 半锥角（°），用于 Parker 权重计算

    // ── [3] 探测器面板 ─────────────────────────────────────────────────────
    int   detector_cols   = 512;    ///< 探测器列数（像素）
    int   detector_rows   = 512;    ///< 探测器行数（像素）
    float detector_size_cm= 40.0f;  ///< 探测器物理宽度（cm），用于推算像素尺寸

    /// 准直遮挡裁剪（像素数）——被裁区域投影值清零，不参与重建
    int   skip_cols_left  = 0;
    int   skip_cols_right = 0;
    int   skip_rows_top   = 0;
    int   skip_rows_bottom= 0;

    // ── [4] 输入文件路径 ───────────────────────────────────────────────────
    std::string gain_file;         ///< 增益（平场）图像路径（HIS 格式）
    std::string frame_info_file;   ///< 帧角度/编号文件路径
    std::string ini_config_file;   ///< .ini 配置文件路径（RECONSTRUCTION 节）
    std::string image_dir;         ///< 2D 投影图像所在目录

    // ── [5] 滤波与后处理 ───────────────────────────────────────────────────
    /// 滤波器类型（不区分大小写）：
    ///   "RamLak"  ── 纯斜坡滤波，锐化最强，噪声最大
    ///   "Hamming" ── Hamming 窗平滑，噪声居中
    ///   "Wiener"  ── Wiener 自适应滤波，低噪声，需配置 filter_param_a / filter_snr
    std::string filter        = "Wiener";

    float filter_param_a      = 0.5f;   ///< Wiener 滤波参数 A（调节截止强度）
    float filter_snr          = 60.0f;  ///< Wiener 信噪比估计（越大越接近 RamLak）

    /// 短扫描（short-scan）标志：
    ///   true  ── 扫描弧 < 360°，启用 Parker 冗余权重 + 距离修正
    ///   false ── 全圆扫描，无需 Parker 权重
    bool  short_scan          = false;

    /// 角步距权重（Δθ weighting）：
    ///   true  ── 每帧反投影前乘以相邻帧角步距，补偿采样不均匀性
    ///   false ── 各帧权重相同（均匀采样假设）
    bool  weight_bp           = true;

    bool  interpolate         = true;   ///< 反投影时使用双线性插值（否则最近邻）
    int   use_ints            = 0;      ///< 保留字段：0=float 体素，1=short 体素

    float angle_weight_min_deg = 0.01f; ///< 角步距权重下限截断（°），防止零权重
    float angle_weight_max_deg = 2.0f;  ///< 角步距权重上限截断（°），防止异常大角步

    int projection_edge_taper_pixels = 0; ///< 投影边缘余弦渐隐像素数（0 = 关闭）
    std::string custom_half_fan_weight; ///< 自定义half-fan冗余权重模式（空/off = 关闭）
    float custom_half_fan_width_pixels = 0.0f; ///< 自定义half-fan权重宽度（像素，<=0 自动）
    float custom_half_fan_strength = 1.0f; ///< 自定义half-fan权重强度（0..1）

    bool rtk_transpose_projections = false; ///< RTK前转置投影像素，匹配AVL FIELD_TRANSPOSE流程
    std::string rtk_explicit_geometry_axis; ///< x/y/z时使用显式RTK源/探测器几何
    std::string rtk_explicit_row_axis; ///< 显式几何探测器row基向量（如 +x）
    std::string rtk_explicit_col_axis; ///< 显式几何探测器col基向量（如 +y）

    // ── [6] 校正因子 ───────────────────────────────────────────────────────
    float air_value           = 60000.0f; ///< 增益校正后的空气期望像素值（ADU 计数）
    float scale_out           = 1000.0f;  ///< 输出缩放因子（HU = voxel × scale_out + offset_out）
    float offset_out          = -1000.0f; ///< 输出偏移（空气 ≈ -1000 HU）
    float beam_hardening_coeff= 0.0f;     ///< 射束硬化校正指数（0 = 关闭）
    float scatter_sigma_cm    = 0.0f;     ///< 散射核高斯标准差（cm，0 = 关闭）
    float ghost_coeff         = 0.0f;     ///< 图像拖尾系数（0 = 关闭）

    /// 非负值截断阈值：重建后体素值低于此值将强制设为该值。
    /// 设为 0 可去除空气中的振铃负值；设为 -1000 可保留 HU 空气绝对值。
    float non_negativity_threshold = 0.0f;

    // ── [7] 多线程配置 ─────────────────────────────────────────────────────
    int   num_threads         = 0; ///< 反投影并行线程数；<=0 时自动使用硬件并发线程数
    int   block_size          = 8; ///< 每个线程处理的连续 Z 切片数
};


// ============================================================================
// ReconProgress  ── 重建进度回调数据
//
// 重建引擎通过 ProgressCallback 定期将进度推送给 UI 线程。
// pct 范围 0..100，finished/error 互斥置位。
// ============================================================================
struct ReconProgress {
    int    frames_done  = 0;     ///< 已处理帧数
    int    frames_total = 0;     ///< 总帧数
    float  pct          = 0.0f;  ///< 进度百分比（0..100）
    std::string message;         ///< 当前阶段描述文本
    bool   finished     = false; ///< true 表示重建正常完成
    bool   error        = false; ///< true 表示发生错误（message 含错误信息）
};

/// 进度回调函数类型（线程安全：从工作线程调用，接收方需做同步）
using ProgressCallback = void(*)(const ReconProgress&, void* user_data);

} // namespace fdk
