#pragma once

#include <functional>
#include <string>
#include <vector>

#include "registration/registration_types.h"
#include "registration/simplex.h"
#include "registration/transform.h"

namespace registration {

// 本文件定义配准内核接口：
// - 输入：两幅或三幅体数据（scan1/scan2/scan3）与先验变换；
// - 输出：最优 Adjust2 变换矩阵；
// - 优化器：Nelder-Mead Simplex（见 simplex.*）；
// - 代价函数：由 metric_ 选择（NMI/相关比/RMS 等）。

// 代价函数类型（与历史 Delphi TMetric 对齐）
enum class Metric {
    kMutualInfo,
    kRmsAdjust,
    kRmdDiff,
    kMaxProduct,
    kMinProduct,
    kNormMutualInfo,
    kCorrRatio
};

// 变换参数化类型（与历史 Delphi TTransformType 对齐）
enum class TransformType {
    kShift,              // 3 params: dx dy dz (in 1/10 mm units)
    kShiftX,             // 1 param:  dx (in 1/10 mm units)
    kShiftY,             // 1 param:  dy (in 1/10 mm units)
    kShiftZ,             // 1 param:  dz (in 1/10 mm units)
    kShiftXZ,            // 2 params: dx dz
    kShiftRot,           // 6 params: dx dy dz rx ry rz
    kRotate,             // 3 params: rx ry rz
    kShiftRotMagn,       // 7 params: dx dy dz rx ry rz iso-scale
    kShiftRotStretch,    // 9 params: dx dy dz rx ry rz sx sy sz
    kMagn,               // 1 param:  iso-scale
    kStretch             // 3 params: sx sy sz
};

// 运行状态（与历史 Delphi TMatchState 对齐）
enum class MatchState {
    kNotCalculated,
    kMissingInfo,
    kCalculating,
    kFinishedError,
    kFinishedOk
};

// 配准类（Delphi TMatch 的 C++ 对应实现）
// 管线关系：Scan1 = inv[T1] inv[A1] [A2] [T2] Scan2
// 其中 A2 是待优化输出。
class Registration {
public:
    using ProgressFn = std::function<void(double cf_value, int evals, const std::string& info)>;

    Registration();

    // 输入体数据
    void set_scan1(Image3D scan);
    void set_scan2(Image3D scan);
    void set_scan3(Image3D scan);  // optional 2D third scan for 3D/2D matching

    // 预变换（在优化前已知）
    void set_transform1(Transform t1);
    void set_transform2(Transform t2);
    void set_transform3(Transform t3);

    // 可选初值（作为 pre_adjust2 进入优化）
    void set_adjust1(Transform a1);
    void set_adjust2(Transform a2);

    // 可选旋转中心（若不设置则使用 scan1 中心）
    void set_rotation_point(std::array<float, 3> p);
    void clear_rotation_point();

    // 模式开关
    void set_fix_scan2(bool value);         // swap fixed/moving
    void set_exclude_zeros(bool value);     // treat 0 as outside (default true)
    void set_post_process(bool value);      // apply preprocess to fixed scan
    void set_chamfer_rms(bool value);       // use RMS instead of mean for chamfer
    void set_force_fast_interpolator(bool value);
    void set_sample_stride(int stride);     // sparse sampling for metric evaluation

    // 优化参数
    void set_metric(Metric metric);
    void set_transform_type(TransformType type);
    void set_param1(double p);  // simplex tolerance   (-1 => default 0.0001)
    void set_param2(double p);  // simplex offset val  (-1 => default 3 or 5)

    // 每次找到更优 cost 时触发回调
    void set_on_lower_func_val(ProgressFn fn);

    // 执行配准并返回最优 A2
    Transform run();

    // 线程外中断
    void stop();

    MatchState state() const;
    double start_cf_value() const;
    double end_cf_value() const;
    double cf_value() const;
    int    eval_count() const;
    const std::string& information() const;

    // 获取当前 A2 下的 moving 图像（用于可视化）
    Image3D get_moving_image() const;

    // Static helper algorithms exposed for module-level wrapper functions.
    static Image3D pre_process(const Image3D& img);
    static Image3D create_distance_map(const Image3D& seg, int segment_value, bool exclude_zeros);
    static Image3D create_dot_list(const Image3D& seg, int segment_value);

private:
    // 各变换类型参数维度
    static int param_count(TransformType type);

    // 将参数向量映射为 A2 矩阵
    Transform build_a2(const std::vector<double>& p) const;

    // 在 dst 网格上对 src 进行三线性重采样（按 t 变换）
    Image3D interpolate(const Image3D& src, const Image3D& dst, const Transform& t) const;
    Image3D interpolate_sampled(const Image3D& src, const Image3D& dst, const Transform& t, int stride) const;
    Image3D sample_image(const Image3D& src, int stride) const;

    // 计算对齐后代价
    double compute_metric(const Image3D& img1, const Image3D& img2) const;

    // Chamfer 距离（点集到距离图）
    double chamfer_cost(const Image3D& dots, const Image3D& dmap) const;

    double cost_function(const std::vector<double>& p);
    void   notify_lower(int evals, double cf, const std::vector<double>& p);

    // --- 管线状态 ---
    Image3D scan1_, scan2_, scan3_;
    Image3D g_scan1_, g_scan2_, g_scan3_;  // working copies after preprocess

    Transform t1_, t2_, t3_;
    Transform a1_, a2_, pre_adjust2_;
    bool has_t1_ = false, has_t2_ = false, has_t3_ = false;
    bool has_a1_ = false, has_a2_ = false;

    std::array<float, 3> rotation_point_{};
    bool has_rotation_point_ = false;

    bool fix_scan2_ = false;
    bool exclude_zeros_ = true;
    bool post_process_ = false;
    bool chamfer_rms_ = false;
    bool force_fast_interpolator_ = true;
    int sample_stride_ = 1;

    Metric metric_ = Metric::kCorrRatio;
    TransformType transform_type_ = TransformType::kShiftRot;
    double param1_ = -1.0;
    double param2_ = -1.0;

    ProgressFn progress_fn_;
    Simplex* simplex_ = nullptr;  // 内部临时持有，仅 run() 生命周期有效

    MatchState state_ = MatchState::kNotCalculated;
    double cf_value_ = 0.0;
    double start_cf_value_ = 0.0;
    double end_cf_value_ = 0.0;
    int    eval_count_ = 0;
    std::string information_;
    Transform result_a2_;
};

// 模块级辅助函数（历史过程式接口的等价实现）

// 从分割体生成点列表（1D 不规则点集）；segment_value<=0 时自动阈值。
Image3D create_dot_list(const Image3D& input, int& segment_value, bool auto_calc = true,
                        int min_dot_count = 10);

// 从分割体生成 chamfer 距离图。
Image3D create_distance_map(const Image3D& input, int& segment_value, bool auto_calc = true,
                             bool exclude_zeros = false);

// 显式阈值区间版本。
Image3D create_dot_list2(const Image3D& input, int& min_seg, int max_seg = -1,
                          int min_dot_count = 10);
Image3D create_distance_map2(const Image3D& input, int& min_seg, int max_seg = -1,
                              bool exclude_zeros = false);

// 强度裁剪到 98 百分位（对应 preProcessScan 语义）。
Image3D pre_process_scan(const Image3D& input);

}  // namespace registration


