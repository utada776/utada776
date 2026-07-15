#pragma once

// ============================================================================
// fdk_recon.h
//
// RTK-backed FDK（Feldkamp-Davis-Kress）锥形束 CT 重建引擎接口。
//
// 本文件对外暴露三层接口：
//   1. 工具函数层   ── IniConfig、LoadFrameInfo、LoadProjections
//                     负责从磁盘读取参数/帧信息/投影图像
//   2. 引擎类层     ── FdkEngine 将投影序列转换为 RTK/ITK 图像并调用 RTK FDK
//
// 算法参考：
//   Feldkamp L.A., Davis L.C., Kress J.W. (1984)
//   "Practical cone-beam algorithm", J. Opt. Soc. Am. A, 1(6), 612-619.
//
// 实际重建路径：rtk::FDKConeBeamReconstructionFilter。
// ============================================================================

#include "FDK recon/fdk_types.h"

#include <functional>
#include <string>
#include <vector>

namespace fdk {

// ============================================================================
// IniConfig  ── 轻量级 INI 文件读取器
//
// 读取 XVI 风格的 .ini 配置文件（[SECTION] / key=value 格式）。
// apply_to_params() 可将 [RECONSTRUCTION] 节的参数直接写入 FdkParams。
// ============================================================================
class IniConfig {
public:
    /// 从指定路径加载 .ini 文件，成功返回 true
    bool load(const std::string& path);

    /// 读取字符串值，键不存在时返回 default_val
    std::string get_string (const std::string& section, const std::string& key,
                            const std::string& default_val = "") const;
    /// 读取整型值，解析失败时返回 default_val
    int         get_int    (const std::string& section, const std::string& key,
                            int         default_val = 0)    const;
    /// 读取浮点值，解析失败时返回 default_val
    float       get_float  (const std::string& section, const std::string& key,
                            float       default_val = 0.0f) const;
    /// 读取布尔值（支持 0/1/true/false/yes/no），解析失败时返回 default_val
    bool        get_bool   (const std::string& section, const std::string& key,
                            bool        default_val = false) const;

    /// 将 [RECONSTRUCTION] 节的参数合并写入 params
    void apply_to_params(FdkParams& params) const;

private:
    // 内部存储：section -> [(key, value), ...]
    std::vector<std::pair<
        std::string,
        std::vector<std::pair<std::string, std::string>>>> m_sections;
};


// ============================================================================
// LoadFrameInfo  ── 帧角度信息加载
//
// 从文本文件（或 XML 文件）中读取每帧的机架角度和帧编号。
// 支持格式：
//   - 两列文本："frame_id  angle_deg"
//   - 单列文本：仅 angle_deg（自动递增帧 ID）
//   - XVI XML：含 <GantryAngle> 标签的简单 XML
// 成功返回 true，失败时 error 含错误描述。
// ============================================================================
bool LoadFrameInfo(const std::string& path,
                   std::vector<FrameInfo>& out,
                   std::string& error);

// ============================================================================
// LoadProjections  ── 投影图像批量加载
//
// 从 params.image_dir 目录按帧列表顺序读取 HIS 格式图像，
// 并完成以下预处理：
//   1. 对数变换：pix = -ln(pix / air_value)    （将强度转为线积分）
//   2. 准直裁剪：skip_cols_* / skip_rows_* 区域清零
//
// 每加载一帧调用一次 progress 回调（可为 nullptr）。
// 成功返回 true，失败时 error 含错误描述。
// ============================================================================
bool LoadProjections(const FdkParams& params,
                     const std::vector<FrameInfo>& frames,
                     std::vector<ProjectionImage>& out,
                     std::function<void(int done, int total, const std::string& msg)> progress,
                     std::string& error);

// ============================================================================
// FdkEngine  ── 离线（批量）FDK 重建引擎
//
// 工作流程：
//   Step 1  LoadProjections 得到线积分投影序列
//   Step 2  构造 RTK projection stack 与 ThreeDCircularProjectionGeometry
//   Step 3  RTK 执行 displaced detector / Parker / ramp / FDK backprojection
//   Step 4  PostProcess 做输出 HU 映射与可选截断
//
// 使用方法：
//   FdkEngine engine(params);
//   engine.Reconstruct(projections, volume, progress_callback);
// ============================================================================
class FdkEngine {
public:
    explicit FdkEngine(const FdkParams& params);

    /// 执行完整重建，成功返回 true。
    /// projections：已加载的投影序列（调用前无需滤波）
    /// volume_out：输出体，函数内自动分配
    /// progress：进度回调（可为 nullptr）
    bool Reconstruct(const std::vector<ProjectionImage>& projections,
                     Volume3D& volume_out,
                     std::function<void(const ReconProgress&)> progress);

    /// 后处理（HU 映射与可选截断）。
    void PostProcess(Volume3D& vol, bool clamp_non_negative = false) const;

private:
    FdkParams m_params; ///< 重建参数（构造时拷贝，生命周期内不变）
};

} // namespace fdk
