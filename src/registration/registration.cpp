#include "registration/registration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>

namespace registration {

// ============================================================================
// registration.cpp  ── 配准内核实现
//
// 核心流程：
//   1. build_a2(p)         ── 将优化参数向量 p 映射为 4×4 变换矩阵 A2
//   2. interpolate()        ── 通过复合变换 T = inv[T1] inv[A1] A2 T2
//                              将 moving 图像三线性重采样到 fixed 网格
//   3. compute_metric()     ── 按选定相似度度量计算 cost（越小越好）
//   4. Simplex 迭代         ── Nelder-Mead 无梯度优化，持续记录最优参数
//
// 支持的相似度度量（metric_）：
//   kCorrRatio     ── 相关比（Correlation Ratio），多模态首选
//   kNormMutualInfo── 归一化互信息（NMI），多模态鲁棒
//   kMutualInfo    ── 互信息（当前用 NMI 近似）
//   kRmsAdjust     ── 均方根误差（骨性配准）
//   kRmdDiff       ── 平均绝对差
//   kMaxProduct / kMinProduct ── 最大/最小乘积（特殊场景）
//
// 支持的配准模式：
//   常规体-体（3D/3D）    ── interpolate() + compute_metric()
//   点集-体（Chamfer）    ── 点集变换 + chamfer_cost()
//   可选第三幅图像约束    ── 3D/2D 或多图配准场景
// ============================================================================

namespace {

// ── 内部匿名命名空间（模块私有实现）──────────────────────────────────────

// 三线性插值：在体数据中采样连续坐标 (xf, yf, zf)。
// 若 honour_border=true 且越界/邻域含零值，则返回 NaN（上层转为 0，等同体外）。
// 三线性插值公式：
//   v = Σ v_ijk × (1±dx)(1±dy)(1±dz)，对 8 个邻域角点求加权和。
float trilinear(const Image3D& vol, float xf, float yf, float zf, bool honour_border) {
    const int x0 = static_cast<int>(std::floor(xf));
    const int y0 = static_cast<int>(std::floor(yf));
    const int z0 = static_cast<int>(std::floor(zf));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const int z1 = z0 + 1;

    if (x0 < 0 || y0 < 0 || z0 < 0 || x1 >= vol.size.x || y1 >= vol.size.y || z1 >= vol.size.z) {
        return honour_border ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
    }

    const float dx = xf - static_cast<float>(x0);
    const float dy = yf - static_cast<float>(y0);
    const float dz = zf - static_cast<float>(z0);
    const float ndx = 1.0f - dx;
    const float ndy = 1.0f - dy;
    const float ndz = 1.0f - dz;

    const float v000 = vol.at(x0, y0, z0);
    const float v100 = vol.at(x1, y0, z0);
    const float v010 = vol.at(x0, y1, z0);
    const float v110 = vol.at(x1, y1, z0);
    const float v001 = vol.at(x0, y0, z1);
    const float v101 = vol.at(x1, y0, z1);
    const float v011 = vol.at(x0, y1, z1);
    const float v111 = vol.at(x1, y1, z1);

    // 约定 0 为“体外”时，邻域有 0 则认为该采样点无效。
    if (honour_border) {
        if (v000 == 0.0f || v100 == 0.0f || v010 == 0.0f || v110 == 0.0f ||
            v001 == 0.0f || v101 == 0.0f || v011 == 0.0f || v111 == 0.0f) {
            return std::numeric_limits<float>::quiet_NaN();
        }
    }

    return v000 * ndx * ndy * ndz + v100 * dx * ndy * ndz +
           v010 * ndx * dy  * ndz + v110 * dx * dy  * ndz +
           v001 * ndx * ndy * dz  + v101 * dx * ndy * dz  +
           v011 * ndx * dy  * dz  + v111 * dx * dy  * dz;
}

// 使用 4x4 齐次矩阵变换三维点，返回 xyz 部分。
std::array<float, 3> apply_transform(const Transform& m, float x, float y, float z) {
    return {m.get(0, 0) * x + m.get(0, 1) * y + m.get(0, 2) * z + m.get(0, 3),
            m.get(1, 0) * x + m.get(1, 1) * y + m.get(1, 2) * z + m.get(1, 3),
            m.get(2, 0) * x + m.get(2, 1) * y + m.get(2, 2) * z + m.get(2, 3)};
}

// 相关比（Correlation Ratio, Roche et al. 1998）。
// 思路：将 fixed 图像（a）分 kBins 个强度箱，计算 moving（b）在各箱内的条件方差，
//   CR = 1 - Σ var_cond / var_total
// 值越接近 1 表示两图像越相关（越相似）。
// 本实现返回相似度，外层取反（1-CR）转为"越小越好"的 cost。
double corr_ratio(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty()) {
        return 1.0;
    }
    constexpr int kBins = 32;
    std::vector<double> sum_b(kBins, 0.0), sum_b2(kBins, 0.0);
    std::vector<int>    cnt(kBins, 0);

    if (a.empty()) {
        return 1.0;
    }
    float a_min = *std::min_element(a.begin(), a.end());
    float a_max = *std::max_element(a.begin(), a.end());
    if (a_max <= a_min) {
        return 1.0;
    }
    const double scale = (kBins - 1) / static_cast<double>(a_max - a_min);

    for (std::size_t i = 0; i < a.size(); ++i) {
        const int bin = static_cast<int>((a[i] - a_min) * scale);
        const int clamped = std::clamp(bin, 0, kBins - 1);
        sum_b[clamped] += b[i];
        sum_b2[clamped] += static_cast<double>(b[i]) * b[i];
        ++cnt[clamped];
    }

    double var_cond = 0.0, total_weight = 0.0;
    for (int k = 0; k < kBins; ++k) {
        if (cnt[k] < 2) {
            continue;
        }
        const double mean_k = sum_b[k] / cnt[k];
        var_cond += sum_b2[k] - static_cast<double>(cnt[k]) * mean_k * mean_k;
        total_weight += cnt[k];
    }
    if (total_weight < 2.0) {
        return 1.0;
    }

    double mean_b = 0.0;
    for (float v : b) {
        mean_b += v;
    }
    mean_b /= static_cast<double>(b.size());

    double var_b = 0.0;
    for (float v : b) {
        var_b += (v - mean_b) * (v - mean_b);
    }
    if (var_b < 1e-10) {
        return 1.0;
    }
    return 1.0 - var_cond / var_b;
}

// 归一化互信息（Normalized Mutual Information, Studholme et al. 1999）。
// 思路：联合直方图估计联合分布，NMI = (H(a) + H(b)) / H(a,b)
// 值 ≥ 1，越大表示两图像越相关（越相似）。多模态配准的鲁棒性强。
// 外层取倒数 1/(NMI+ε) 转为 cost。
double norm_mutual_info(const std::vector<float>& a, const std::vector<float>& b) {
    constexpr int kBins = 32;
    if (a.empty()) {
        return 0.0;
    }
    auto rescale = [kBins](const std::vector<float>& v) {
        const float mn = *std::min_element(v.begin(), v.end());
        const float mx = *std::max_element(v.begin(), v.end());
        std::vector<int> idx(v.size());
        const double sc = (kBins - 1) / static_cast<double>(mx - mn + 1e-9f);
        for (std::size_t i = 0; i < v.size(); ++i) {
            idx[i] = std::clamp(static_cast<int>((v[i] - mn) * sc), 0, kBins - 1);
        }
        return idx;
    };

    auto ia = rescale(a);
    auto ib = rescale(b);

    std::vector<double> joint(kBins * kBins, 0.0);
    std::vector<double> ha(kBins, 0.0), hb(kBins, 0.0);
    const double w = 1.0 / static_cast<double>(a.size());

    for (std::size_t i = 0; i < a.size(); ++i) {
        joint[ia[i] * kBins + ib[i]] += w;
        ha[ia[i]] += w;
        hb[ib[i]] += w;
    }

    auto entropy = [&](const std::vector<double>& p) {
        double e = 0.0;
        for (double v : p) {
            if (v > 1e-12) {
                e -= v * std::log(v);
            }
        }
        return e;
    };

    const double Ha = entropy(ha);
    const double Hb = entropy(hb);
    const double Hab = entropy(joint);
    if (Hab < 1e-12) {
        return 0.0;
    }
    return (Ha + Hb) / Hab;
}

// 均方根误差（Root Mean Square Error）。
// 适用于同模态（如骨性区域灰度直接对比）配准；值越小越对齐。
double rms_adjust(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = a[i] - b[i];
        sum += d * d;
    }
    return std::sqrt(sum / static_cast<double>(a.size()));
}

// Chamfer 3D 距离变换（Borgefors 1986，3-4-5 整数权重近似欧氏距离）。
// 两遍扫描（前向 + 后向）在体内高效传播最短距离。
// 权重约定（体素单位）：
//   面相邻（6-邻域） = 3  ≈ 1.0 × 3
//   棱相邻（12-邻域）= 4  ≈ √2 × 3 ≈ 4.24
//   角相邻（8-邻域） = 5  ≈ √3 × 3 ≈ 5.20
// 用于 Chamfer 配准：将分割体生成距离图，再将点集采样其中求平均距离作为 cost。
Image3D chamfer_3d(const Image3D& seg, float threshold) {
    const int nx = seg.size.x;
    const int ny = seg.size.y;
    const int nz = seg.size.z;
    Image3D dm;
    dm.size = seg.size;
    dm.voxels.assign(seg.voxel_count(), 1e9f);

    auto idx = [&](int x, int y, int z) {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) +
               static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
    };

    // Seed
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                if (seg.at(x, y, z) >= threshold) {
                    dm.voxels[idx(x, y, z)] = 0.0f;
                }
            }
        }
    }

    // Forward pass  (Borgefors weights: face=3, edge=4, corner=5)
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                float& cur = dm.voxels[idx(x, y, z)];
                if (x > 0)          cur = std::min(cur, dm.voxels[idx(x-1,y,z)] + 3.0f);
                if (y > 0)          cur = std::min(cur, dm.voxels[idx(x,y-1,z)] + 3.0f);
                if (z > 0)          cur = std::min(cur, dm.voxels[idx(x,y,z-1)] + 3.0f);
                if (x > 0 && y > 0) cur = std::min(cur, dm.voxels[idx(x-1,y-1,z)] + 4.0f);
                if (x > 0 && z > 0) cur = std::min(cur, dm.voxels[idx(x-1,y,z-1)] + 4.0f);
                if (y > 0 && z > 0) cur = std::min(cur, dm.voxels[idx(x,y-1,z-1)] + 4.0f);
                if (x > 0 && y > 0 && z > 0) cur = std::min(cur, dm.voxels[idx(x-1,y-1,z-1)] + 5.0f);
            }
        }
    }

    // Backward pass
    for (int z = nz-1; z >= 0; --z) {
        for (int y = ny-1; y >= 0; --y) {
            for (int x = nx-1; x >= 0; --x) {
                float& cur = dm.voxels[idx(x, y, z)];
                if (x+1 < nx)                   cur = std::min(cur, dm.voxels[idx(x+1,y,z)] + 3.0f);
                if (y+1 < ny)                   cur = std::min(cur, dm.voxels[idx(x,y+1,z)] + 3.0f);
                if (z+1 < nz)                   cur = std::min(cur, dm.voxels[idx(x,y,z+1)] + 3.0f);
                if (x+1 < nx && y+1 < ny)       cur = std::min(cur, dm.voxels[idx(x+1,y+1,z)] + 4.0f);
                if (x+1 < nx && z+1 < nz)       cur = std::min(cur, dm.voxels[idx(x+1,y,z+1)] + 4.0f);
                if (y+1 < ny && z+1 < nz)       cur = std::min(cur, dm.voxels[idx(x,y+1,z+1)] + 4.0f);
                if (x+1 < nx && y+1 < ny && z+1 < nz) cur = std::min(cur, dm.voxels[idx(x+1,y+1,z+1)] + 5.0f);
            }
        }
    }
    return dm;
}

// ---------------------------------------------------------------------------
// 98th-percentile clip (preProcessScan)
// ---------------------------------------------------------------------------
Image3D do_pre_process(const Image3D& in) {
    Image3D out = in;
    std::vector<float> v = in.voxels;
    std::sort(v.begin(), v.end());
    const float p98 = v[static_cast<std::size_t>(v.size() * 98 / 100)];
    if (p98 <= 0.0f) {
        return out;
    }
    for (float& px : out.voxels) {
        if (px > p98) {
            px = p98;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Auto-segment threshold: 99th percentile
// ---------------------------------------------------------------------------
int auto_threshold(const Image3D& img) {
    std::vector<float> v = img.voxels;
    std::sort(v.begin(), v.end());
    return static_cast<int>(v[static_cast<std::size_t>(v.size() * 99 / 100)]);
}

}  // anonymous namespace

// ===========================================================================
//  Registration 成员函数实现
// ===========================================================================

Registration::Registration() = default;

void Registration::set_scan1(Image3D scan) { scan1_ = std::move(scan); }
void Registration::set_scan2(Image3D scan) { scan2_ = std::move(scan); }
void Registration::set_scan3(Image3D scan) { scan3_ = std::move(scan); }

void Registration::set_transform1(Transform t) { t1_ = t; has_t1_ = true; }
void Registration::set_transform2(Transform t) { t2_ = t; has_t2_ = true; }
void Registration::set_transform3(Transform t) { t3_ = t; has_t3_ = true; }

void Registration::set_adjust1(Transform a)  { a1_ = a; has_a1_ = true; }
void Registration::set_adjust2(Transform a)  { a2_ = a; has_a2_ = true; }

void Registration::set_rotation_point(std::array<float, 3> p) { rotation_point_ = p; has_rotation_point_ = true; }
void Registration::clear_rotation_point() { has_rotation_point_ = false; }

void Registration::set_fix_scan2(bool v)                { fix_scan2_ = v; }
void Registration::set_exclude_zeros(bool v)            { exclude_zeros_ = v; }
void Registration::set_post_process(bool v)             { post_process_ = v; }
void Registration::set_chamfer_rms(bool v)              { chamfer_rms_ = v; }
void Registration::set_force_fast_interpolator(bool v)  { force_fast_interpolator_ = v; }
void Registration::set_sample_stride(int v)             { sample_stride_ = std::max(1, v); }
void Registration::set_metric(Metric m)                 { metric_ = m; }
void Registration::set_transform_type(TransformType t)  { transform_type_ = t; }
void Registration::set_param1(double p)                 { param1_ = p; }
void Registration::set_param2(double p)                 { param2_ = p; }
void Registration::set_on_lower_func_val(ProgressFn fn) { progress_fn_ = std::move(fn); }

MatchState Registration::state() const     { return state_; }
double     Registration::start_cf_value() const  { return start_cf_value_; }
double     Registration::end_cf_value() const    { return end_cf_value_; }
double     Registration::cf_value() const        { return cf_value_; }
int        Registration::eval_count() const      { return eval_count_; }
const std::string& Registration::information() const { return information_; }

// 各 transform_type 对应参数维度。
int Registration::param_count(TransformType type) {
    switch (type) {
        case TransformType::kShift:            return 3;
        case TransformType::kShiftX:           return 1;
        case TransformType::kShiftY:           return 1;
        case TransformType::kShiftZ:           return 1;
        case TransformType::kShiftXZ:          return 2;
        case TransformType::kShiftRot:         return 6;
        case TransformType::kRotate:           return 3;
        case TransformType::kShiftRotMagn:     return 7;
        case TransformType::kShiftRotStretch:  return 9;
        case TransformType::kMagn:             return 1;
        case TransformType::kStretch:          return 3;
    }
    return 6;
}

// 将优化器参数映射为 A2 矩阵。
// - 平移参数单位是 0.1mm（因此 /10）；
// - 缩放参数单位是百分数（因此 /100）；
// - 最终围绕旋转中心做“平移到中心 -> 变换 -> 平移回原位”的夹心变换。
Transform Registration::build_a2(const std::vector<double>& p) const {
    Transform a2_fine;

    switch (transform_type_) {
        case TransformType::kShift:
            a2_fine.make_translation(static_cast<float>(p[0] / 10.0),
                                     static_cast<float>(p[1] / 10.0),
                                     static_cast<float>(p[2] / 10.0));
            break;
        case TransformType::kShiftX:
            a2_fine.make_translation(static_cast<float>(p[0] / 10.0),
                                     0.0f,
                                     0.0f);
            break;
        case TransformType::kShiftY:
            a2_fine.make_translation(0.0f,
                                     static_cast<float>(p[0] / 10.0),
                                     0.0f);
            break;
        case TransformType::kShiftZ:
            a2_fine.make_translation(0.0f,
                                     0.0f,
                                     static_cast<float>(p[0] / 10.0));
            break;
        case TransformType::kShiftXZ:
            a2_fine.make_translation(static_cast<float>(p[0] / 10.0), 0.0f,
                                     static_cast<float>(p[1] / 10.0));
            break;
        case TransformType::kRotate:
            a2_fine.make_rotation(static_cast<float>(p[0]), static_cast<float>(p[1]),
                                  static_cast<float>(p[2]));
            break;
        case TransformType::kShiftRot:
        case TransformType::kShiftRotMagn:
        case TransformType::kShiftRotStretch: {
            Transform t;
            t.make_translation(static_cast<float>(p[0] / 10.0),
                                static_cast<float>(p[1] / 10.0),
                                static_cast<float>(p[2] / 10.0));
            Transform r;
            r.make_rotation(static_cast<float>(p[3]), static_cast<float>(p[4]),
                             static_cast<float>(p[5]));
            a2_fine = t;
            a2_fine.pre_multiply(r);

            if (transform_type_ == TransformType::kShiftRotMagn && p.size() >= 7) {
                Transform s;
                const float iso = static_cast<float>(p[6] / 100.0);
                s.make_scaling(iso, iso, iso);
                a2_fine.pre_multiply(s);
            } else if (transform_type_ == TransformType::kShiftRotStretch && p.size() >= 9) {
                Transform s;
                s.make_scaling(static_cast<float>(p[6] / 100.0),
                                static_cast<float>(p[7] / 100.0),
                                static_cast<float>(p[8] / 100.0));
                a2_fine.pre_multiply(s);
            }
            break;
        }
        case TransformType::kMagn: {
            const float iso = static_cast<float>(p[0] / 100.0);
            a2_fine.make_scaling(iso, iso, iso);
            break;
        }
        case TransformType::kStretch:
            a2_fine.make_scaling(static_cast<float>(p[0] / 100.0),
                                  static_cast<float>(p[1] / 100.0),
                                  static_cast<float>(p[2] / 100.0));
            break;
    }

    // 旋转中心夹心：A2 = Pm * A2_fine * inv(Pm)
    float rpx = 0.0f, rpy = 0.0f, rpz = 0.0f;
    if (has_rotation_point_) {
        rpx = rotation_point_[0];
        rpy = rotation_point_[1];
        rpz = rotation_point_[2];
    } else if (!g_scan1_.voxels.empty()) {
        rpx = 0.5f * static_cast<float>(g_scan1_.size.x);
        rpy = 0.5f * static_cast<float>(g_scan1_.size.y);
        rpz = 0.5f * static_cast<float>(g_scan1_.size.z);
    }

    Transform pm;
    pm.make_translation(rpx, rpy, rpz);
    Transform pm_inv = pm;
    pm_inv.invert();

    Transform a2 = pm;
    a2.post_multiply(a2_fine);
    a2.post_multiply(pm_inv);

    // 若传入初值 A2（has_a2_），在当前优化变换前后组合形成 warm start。
    if (has_a2_ && transform_type_ != TransformType::kShiftXZ) {
        a2.pre_multiply(pre_adjust2_);
    } else if (has_a2_ && transform_type_ == TransformType::kShiftXZ) {
        a2.post_multiply(pre_adjust2_);
    }

    return a2;
}

// 将 src 按 t 变换后重采样到 dst 网格。
Image3D Registration::interpolate(const Image3D& src, const Image3D& dst,
                                    const Transform& t) const {
    Image3D out;
    out.size = dst.size;
    out.voxels.assign(dst.voxel_count(), 0.0f);

    const bool border = exclude_zeros_ && !force_fast_interpolator_;

    for (int z = 0; z < dst.size.z; ++z) {
        for (int y = 0; y < dst.size.y; ++y) {
            for (int x = 0; x < dst.size.x; ++x) {
                const auto sp = apply_transform(t, static_cast<float>(x),
                                                 static_cast<float>(y),
                                                 static_cast<float>(z));
                const float v = trilinear(src, sp[0], sp[1], sp[2], border);
                const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(dst.size.x) *
                                      static_cast<std::size_t>(dst.size.y) +
                                      static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.size.x) +
                                      static_cast<std::size_t>(x);
                out.voxels[i] = std::isnan(v) ? 0.0f : v;
            }
        }
    }
    return out;
}

Image3D Registration::sample_image(const Image3D& src, int stride) const {
    const int s = std::max(1, stride);
    Image3D out;
    out.size = {
        (src.size.x + s - 1) / s,
        (src.size.y + s - 1) / s,
        (src.size.z + s - 1) / s,
    };
    out.voxels.assign(out.voxel_count(), 0.0f);

    std::size_t index = 0;
    for (int z = 0; z < src.size.z; z += s) {
        for (int y = 0; y < src.size.y; y += s) {
            for (int x = 0; x < src.size.x; x += s) {
                out.voxels[index++] = src.at(x, y, z);
            }
        }
    }
    return out;
}

Image3D Registration::interpolate_sampled(const Image3D& src, const Image3D& dst,
                                          const Transform& t, int stride) const {
    // 稀疏采样版本：粗阶段可显著降低评估开销。
    const int s = std::max(1, stride);
    Image3D out;
    out.size = {
        (dst.size.x + s - 1) / s,
        (dst.size.y + s - 1) / s,
        (dst.size.z + s - 1) / s,
    };
    out.voxels.assign(out.voxel_count(), 0.0f);

    const bool border = exclude_zeros_ && !force_fast_interpolator_;
    std::size_t index = 0;
    for (int z = 0; z < dst.size.z; z += s) {
        for (int y = 0; y < dst.size.y; y += s) {
            for (int x = 0; x < dst.size.x; x += s) {
                const auto sp = apply_transform(t, static_cast<float>(x),
                                                static_cast<float>(y),
                                                static_cast<float>(z));
                const float v = trilinear(src, sp[0], sp[1], sp[2], border);
                out.voxels[index++] = std::isnan(v) ? 0.0f : v;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Image pre-processing
// ---------------------------------------------------------------------------
Image3D Registration::pre_process(const Image3D& img) {
    return do_pre_process(img);
}

// ---------------------------------------------------------------------------
// Distance map
// ---------------------------------------------------------------------------
Image3D Registration::create_distance_map(const Image3D& seg, int segment_value,
                                            bool exclude_zeros) {
    (void)exclude_zeros;
    return chamfer_3d(seg, static_cast<float>(segment_value));
}

// ---------------------------------------------------------------------------
// Dot list: extract voxel centres above threshold
// ---------------------------------------------------------------------------
Image3D Registration::create_dot_list(const Image3D& seg, int segment_value) {
    Image3D out;
    std::vector<float> pts;
    for (int z = 0; z < seg.size.z; ++z) {
        for (int y = 0; y < seg.size.y; ++y) {
            for (int x = 0; x < seg.size.x; ++x) {
                if (seg.at(x, y, z) >= static_cast<float>(segment_value)) {
                    pts.push_back(static_cast<float>(x));
                    pts.push_back(static_cast<float>(y));
                    pts.push_back(static_cast<float>(z));
                }
            }
        }
    }
    const int n = static_cast<int>(pts.size()) / 3;
    out.size = {n, 3, 1};
    out.voxels = std::move(pts);
    return out;
}

// 统一 metric 计算入口。
// 约定输出均为“越小越好”的 cost，便于 simplex 最小化。
double Registration::compute_metric(const Image3D& img1, const Image3D& img2) const {
    const std::size_t n = std::min(img1.voxels.size(), img2.voxels.size());
    if (n == 0) {
        return 1e18;
    }

    std::vector<float> a(img1.voxels.begin(), img1.voxels.begin() + static_cast<std::ptrdiff_t>(n));
    std::vector<float> b(img2.voxels.begin(), img2.voxels.begin() + static_cast<std::ptrdiff_t>(n));

    if (exclude_zeros_) {
        std::vector<float> fa, fb;
        for (std::size_t i = 0; i < n; ++i) {
            if (a[i] != 0.0f && b[i] != 0.0f) {
                fa.push_back(a[i]);
                fb.push_back(b[i]);
            }
        }
        if (fa.empty()) {
            return 1e18;
        }
        a = std::move(fa);
        b = std::move(fb);
    }

    switch (metric_) {
        case Metric::kCorrRatio: {
            const double cr = corr_ratio(a, b);
            return 1.0 - cr;
        }
        case Metric::kNormMutualInfo: {
            const double nmi = norm_mutual_info(a, b);
            return 1.0 / (nmi + 1e-9);
        }
        case Metric::kMutualInfo: {
            // 当前移植版本中，用 NMI 近似 MI。
            const double nmi = norm_mutual_info(a, b);
            return 1.0 / (nmi + 1e-9);
        }
        case Metric::kRmsAdjust:
            return rms_adjust(a, b);
        case Metric::kRmdDiff: {
            double s = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                s += std::fabs(a[i] - b[i]);
            }
            return s / static_cast<double>(a.size());
        }
        case Metric::kMaxProduct: {
            double mx = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                mx = std::max(mx, static_cast<double>(a[i]) * b[i]);
            }
            return -mx;
        }
        case Metric::kMinProduct: {
            double mn = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < a.size(); ++i) {
                mn = std::min(mn, static_cast<double>(a[i]) * b[i]);
            }
            return mn;
        }
    }
    return 1e18;
}

// ---------------------------------------------------------------------------
// Chamfer cost (1D dot-list probed into distance map)
// Mirrors Delphi: sample dmap at dot positions; cost = mean (or RMS) distance
// of inside points only, normalised by proportion inside.
// ---------------------------------------------------------------------------
double Registration::chamfer_cost(const Image3D& dots, const Image3D& dmap) const {
    if (dots.size.x == 0) {
        return 1000.0;
    }
    double sum = 0.0, count = 0.0, inside = 0.0;
    const int npts = dots.size.x;
    for (int i = 0; i < npts; ++i) {
        const float x = dots.voxels[static_cast<std::size_t>(i) * 3];
        const float y = dots.voxels[static_cast<std::size_t>(i) * 3 + 1];
        const float z = dots.voxels[static_cast<std::size_t>(i) * 3 + 2];
        const float d = trilinear(dmap, x, y, z, false);
        if (d >= 0.0f) {
            sum += chamfer_rms_ ? d * d : d;
            ++count;
        }
        ++inside;
    }
    if (inside == 0.0) {
        return 1000.0;
    }
    if (chamfer_rms_) {
        return 255.0 * std::sqrt(sum / count) / inside;
    }
    return 255.0 * (sum / count) / inside;
}

// 代价函数主入口：每次 simplex 试探参数都会调用。
// 管线：Scan1 = inv[T1] inv[A1] [A2] [T2] Scan2
// 同时支持：
// - 常规体数据 metric；
// - Chamfer（点集 vs 距离图）；
// - 可选第三幅图像的附加约束。
double Registration::cost_function(const std::vector<double>& p) {
    ++eval_count_;

    const Transform a2 = build_a2(p);
    result_a2_ = a2;

    // 组装复合变换 t = inv[T1] inv[A1] A2 T2
    Transform t = a2;

    if (has_a1_) {
        Transform inv_a1 = a1_;
        inv_a1.invert();
        t.pre_multiply(inv_a1);
    }
    if (has_t1_) {
        Transform inv_t1 = t1_;
        inv_t1.invert();
        t.pre_multiply(inv_t1);
    }
    if (has_t2_) {
        t.post_multiply(t2_);
    }

    double cost = 0.0;

    // Chamfer 特判：scan?.size.z==1 被当作点列表结构。
    const bool is_chamfer_fwd = (g_scan1_.size.z == 1 && g_scan2_.size.z > 1);
    const bool is_chamfer_rev = (g_scan2_.size.z == 1 && g_scan1_.size.z > 1);

    if (is_chamfer_fwd && !fix_scan2_) {
        Transform t_inv = t;
        t_inv.invert();
        Image3D warped_dots = g_scan2_;  // shallow copy of dot-list
        // Transform each dot
        const int npts = warped_dots.size.x;
        for (int i = 0; i < npts; ++i) {
            float& x = warped_dots.voxels[static_cast<std::size_t>(i) * 3];
            float& y = warped_dots.voxels[static_cast<std::size_t>(i) * 3 + 1];
            float& z = warped_dots.voxels[static_cast<std::size_t>(i) * 3 + 2];
            const auto np = apply_transform(t, x, y, z);
            x = np[0]; y = np[1]; z = np[2];
        }
        cost = chamfer_cost(warped_dots, g_scan1_);
    } else if (is_chamfer_rev && fix_scan2_) {
        Transform t_inv = t;
        t_inv.invert();
        Image3D warped = g_scan1_;
        const int npts = warped.size.x;
        for (int i = 0; i < npts; ++i) {
            float& x = warped.voxels[static_cast<std::size_t>(i) * 3];
            float& y = warped.voxels[static_cast<std::size_t>(i) * 3 + 1];
            float& z = warped.voxels[static_cast<std::size_t>(i) * 3 + 2];
            const auto np = apply_transform(t_inv, x, y, z);
            x = np[0]; y = np[1]; z = np[2];
        }
        cost = chamfer_cost(warped, g_scan2_);
    } else if (fix_scan2_) {
        Transform t_inv = t;
        t_inv.invert();
        Image3D warped = (sample_stride_ > 1)
            ? interpolate_sampled(g_scan1_, g_scan2_, t_inv, sample_stride_)
            : interpolate(g_scan1_, g_scan2_, t_inv);
        if (post_process_) {
            warped = do_pre_process(warped);
        }
        const Image3D fixed = (sample_stride_ > 1)
            ? sample_image(g_scan2_, sample_stride_)
            : g_scan2_;
        cost = compute_metric(warped, fixed);
    } else {
        Image3D warped = (sample_stride_ > 1)
            ? interpolate_sampled(g_scan2_, g_scan1_, t, sample_stride_)
            : interpolate(g_scan2_, g_scan1_, t);
        if (post_process_) {
            warped = do_pre_process(warped);
        }
        const Image3D fixed = (sample_stride_ > 1)
            ? sample_image(g_scan1_, sample_stride_)
            : g_scan1_;
        cost = compute_metric(warped, fixed);
    }

    // 可选第三幅图像约束（例如 3D/2D 场景）。
    if (!g_scan3_.voxels.empty()) {
        Transform s = a2;
        if (has_t3_) {
            s.post_multiply(t3_);
        }
        if (fix_scan2_) {
            Transform s_inv = s; s_inv.invert();
            Image3D w3 = (sample_stride_ > 1)
                ? interpolate_sampled(g_scan1_, g_scan3_, s_inv, sample_stride_)
                : interpolate(g_scan1_, g_scan3_, s_inv);
            if (post_process_) w3 = do_pre_process(w3);
            const Image3D fixed3 = (sample_stride_ > 1)
                ? sample_image(g_scan3_, sample_stride_)
                : g_scan3_;
            cost += compute_metric(w3, fixed3);
        } else {
            Image3D w3 = (sample_stride_ > 1)
                ? interpolate_sampled(g_scan3_, g_scan1_, s, sample_stride_)
                : interpolate(g_scan3_, g_scan1_, s);
            if (post_process_) w3 = do_pre_process(w3);
            const Image3D fixed3 = (sample_stride_ > 1)
                ? sample_image(g_scan1_, sample_stride_)
                : g_scan1_;
            cost += compute_metric(w3, fixed3);
        }
    }

    cf_value_ = cost;
    if (eval_count_ == 1) {
        start_cf_value_ = cost;
    }
    return cost;
}

// 当发现更低 cost 时记录信息并回调上层（GUI 可实时打印日志）。
void Registration::notify_lower(int evals, double cf, const std::vector<double>& p) {
    end_cf_value_ = cf;
    std::ostringstream oss;
    oss << "cf=" << cf << " evals=" << evals;
    if (!p.empty()) {
        oss << " p=[";
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (i != 0) {
                oss << ",";
            }
            oss << p[i];
        }
        oss << "]";
    }
    information_ = oss.str();
    if (progress_fn_) {
        progress_fn_(cf, evals, information_);
    }
}

// 运行入口：准备数据 -> 配置 simplex -> 优化 -> 输出最优 A2。
Transform Registration::run() {
    state_ = MatchState::kMissingInfo;

    if (scan1_.voxels.empty()) {
        information_ = "Scan1 is empty";
        return result_a2_;
    }
    if (scan2_.voxels.empty()) {
        information_ = "Scan2 is empty";
        return result_a2_;
    }

    state_ = MatchState::kCalculating;
    eval_count_ = 0;
    cf_value_ = 0.0;
    start_cf_value_ = 0.0;
    end_cf_value_ = 0.0;

    // 若用户提供了初始 A2，将其作为 warm start（pre_adjust2_）。
    if (has_a2_) {
        pre_adjust2_ = a2_;
    }

    // 准备工作副本；按设置决定是否做预处理。
    g_scan1_ = (post_process_ && !fix_scan2_) ? do_pre_process(scan1_) : scan1_;
    g_scan2_ = (post_process_ && fix_scan2_) ? do_pre_process(scan2_) : scan2_;
    g_scan3_ = scan3_;
    if (!g_scan3_.voxels.empty() && post_process_ && fix_scan2_) {
        g_scan3_ = do_pre_process(g_scan3_);
    }

    // Simplex 参数：
    // - tol 收敛阈值；
    // - offset 初始单纯形步长（影响搜索覆盖范围与速度）。
    const int dims = param_count(transform_type_);
    const bool is_chamfer = (scan1_.size.z == 1 && scan2_.size.z > 1) ||
                             (scan2_.size.z == 1 && scan1_.size.z > 1);

    const double tol    = (param1_ > 0.0) ? param1_ : 0.0001;
    const double offset = (param2_ > 0.0) ? param2_ : (is_chamfer ? 5.0 : 3.0);

    // 初值向量：平移/旋转默认 0；缩放默认 100（即 1.0 倍）。
    std::vector<double> guess(static_cast<std::size_t>(dims), 0.0);
    if (transform_type_ == TransformType::kMagn || transform_type_ == TransformType::kStretch) {
        std::fill(guess.begin(), guess.end(), 100.0);
    } else if (transform_type_ == TransformType::kShiftRotMagn && dims >= 7) {
        guess[6] = 100.0;
    } else if (transform_type_ == TransformType::kShiftRotStretch && dims >= 9) {
        guess[6] = guess[7] = guess[8] = 100.0;
    }

    Simplex simplex([this](const std::vector<double>& p) { return cost_function(p); });
    simplex.set_tolerance(tol);
    simplex.set_initial_step(offset);
    simplex.set_max_iterations(500);
    simplex_ = &simplex;  // allow stop() to interrupt from another thread

    double last_best = std::numeric_limits<double>::max();
    simplex.set_iteration_callback([&](int it, double cost, const std::vector<double>& current_params) {
        (void)it;
        if (cost < last_best) {
            last_best = cost;
            notify_lower(eval_count_, cost, current_params);
        }
    });

    const std::vector<double> best_params = simplex.optimize(guess);
    end_cf_value_ = simplex.best_cost();
    if (information_.empty()) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << "cf=" << end_cf_value_ << " evals=" << eval_count_;
        if (!best_params.empty()) {
            oss << " p=[";
            for (std::size_t i = 0; i < best_params.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << best_params[i];
            }
            oss << "]";
        }
        information_ = oss.str();
    }
    // 一定要使用 best_params 重建结果：
    // result_a2_ 在迭代中会被“最后一次评估点”覆盖，该点不一定是全局最优点。
    result_a2_ = build_a2(best_params);

    simplex_ = nullptr;
    state_ = MatchState::kFinishedOk;
    return result_a2_;
}

void Registration::stop() {
    // 请求 simplex 在下一轮检查时退出。
    // 这里保持 FinishedOk，便于调用方继续使用“当前最优解”。
    if (simplex_) {
        simplex_->stop();
    }
    state_ = MatchState::kFinishedOk;
}

// ---------------------------------------------------------------------------
// Get the moving image under current A2 (for visualisation)
// ---------------------------------------------------------------------------
Image3D Registration::get_moving_image() const {
    if (g_scan2_.voxels.empty() || g_scan1_.voxels.empty()) {
        return {};
    }
    Transform t = result_a2_;
    if (has_a1_) {
        Transform inv_a1 = a1_; inv_a1.invert();
        t.pre_multiply(inv_a1);
    }
    if (has_t1_) {
        Transform inv_t1 = t1_; inv_t1.invert();
        t.pre_multiply(inv_t1);
    }
    if (has_t2_) t.post_multiply(t2_);
    return interpolate(g_scan2_, g_scan1_, t);
}

// ===========================================================================
//  Module-level helper functions
// ===========================================================================

Image3D create_dot_list(const Image3D& input, int& segment_value, bool auto_calc,
                         int min_dot_count) {
    if (auto_calc || segment_value <= 0) {
        segment_value = auto_threshold(input);
    }
    Image3D dots = Registration::create_dot_list(input, segment_value);
    while (dots.size.x < min_dot_count && segment_value > 1) {
        --segment_value;
        dots = Registration::create_dot_list(input, segment_value);
    }
    return dots;
}

Image3D create_distance_map(const Image3D& input, int& segment_value, bool auto_calc,
                              bool exclude_zeros) {
    if (auto_calc || segment_value <= 0) {
        segment_value = auto_threshold(input);
    }
    return Registration::create_distance_map(input, segment_value, exclude_zeros);
}

Image3D create_dot_list2(const Image3D& input, int& min_seg, int max_seg,
                          int min_dot_count) {
    if (min_seg <= 0) {
        min_seg = auto_threshold(input);
    }
    const int eff_max = (max_seg < 0) ? static_cast<int>(1e9) : max_seg;
    Image3D dots;
    std::vector<float> pts;
    for (int z = 0; z < input.size.z; ++z) {
        for (int y = 0; y < input.size.y; ++y) {
            for (int x = 0; x < input.size.x; ++x) {
                const int v = static_cast<int>(input.at(x, y, z));
                if (v >= min_seg && v <= eff_max) {
                    pts.push_back(static_cast<float>(x));
                    pts.push_back(static_cast<float>(y));
                    pts.push_back(static_cast<float>(z));
                }
            }
        }
    }
    while (static_cast<int>(pts.size()) / 3 < min_dot_count && min_seg > 1) {
        --min_seg;
        pts.clear();
        for (int z = 0; z < input.size.z; ++z) {
            for (int y = 0; y < input.size.y; ++y) {
                for (int x = 0; x < input.size.x; ++x) {
                    const int v = static_cast<int>(input.at(x, y, z));
                    if (v >= min_seg && v <= eff_max) {
                        pts.push_back(static_cast<float>(x));
                        pts.push_back(static_cast<float>(y));
                        pts.push_back(static_cast<float>(z));
                    }
                }
            }
        }
    }
    const int n = static_cast<int>(pts.size()) / 3;
    dots.size = {n, 3, 1};
    dots.voxels = std::move(pts);
    return dots;
}

Image3D create_distance_map2(const Image3D& input, int& min_seg, int max_seg,
                               bool exclude_zeros) {
    if (min_seg <= 0) {
        min_seg = auto_threshold(input);
    }
    const int eff_max = (max_seg < 0) ? static_cast<int>(1e9) : max_seg;
    // Build a binary mask
    Image3D mask = input;
    for (float& v : mask.voxels) {
        const int iv = static_cast<int>(v);
        v = (iv >= min_seg && iv <= eff_max) ? 1.0f : 0.0f;
    }
    (void)exclude_zeros;
    return chamfer_3d(mask, 0.5f);
}

Image3D pre_process_scan(const Image3D& input) {
    return do_pre_process(input);
}

}  // namespace registration


