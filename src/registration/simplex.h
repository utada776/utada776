#pragma once

#include <atomic>
#include <functional>
#include <vector>

namespace registration {

// ============================================================================
// Simplex  ── Nelder-Mead 单纯形优化器
//
// 该优化器不依赖梯度，适合配准这类代价函数不光滑、不可导或噪声较大的场景。
// 约定：目标函数越小越好（最小化问题）。
//
// 算法概述（Nelder & Mead, 1965）：
//   1. 初始化 n+1 个顶点（n = 参数维度），以 initial_step 为扰动量
//   2. 每轮找最优（ilo）、最差（ihi）、次差（inhi）顶点
//   3. 对最差顶点依次尝试：反射 → 扩张 → 内收缩 → 整体收缩
//   4. 用相对误差 rtol < tolerance 判定收敛
//   5. 支持外部中断（stop() 置位原子标志）
//
// 典型用法（配准场景）：
//   Simplex opt(cost_function);
//   opt.set_tolerance(1e-5);
//   opt.set_max_iterations(1000);
//   auto best = opt.optimize(initial_params);
// ============================================================================

class Simplex {
public:
    using CostFunction = std::function<double(const std::vector<double>&)>;
    using IterationCallback = std::function<void(int, double, const std::vector<double>&)>;

    explicit Simplex(CostFunction fn);

    void set_tolerance(double tol);              ///< 收敛阈值（相对误差）
    void set_max_iterations(int max_iterations); ///< 最大迭代次数
    void set_initial_step(double step);          ///< 初始顶点扰动量（参数空间单位）

    void set_iteration_callback(IterationCallback cb); ///< 每轮迭代后触发的回调（可用于日志/早停）

    /// 执行优化，返回最优参数向量
    std::vector<double> optimize(const std::vector<double>& initial_guess);

    void stop(); ///< 线程安全中断：下次迭代前退出

    int iteration_count() const;  ///< 实际迭代次数
    double best_cost() const;     ///< 最优代价值

private:
    CostFunction cost_function_;
    IterationCallback iteration_callback_;
    std::atomic<bool> stop_requested_{false};
    int max_iterations_ = 500;
    int iterations_ = 0;
    double tolerance_ = 1e-7;
    double initial_step_ = 1.0;
    double best_cost_ = 0.0;
};

}  // namespace registration


