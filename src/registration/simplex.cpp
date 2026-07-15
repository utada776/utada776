#include "registration/simplex.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace registration {

// Simplex 实现要点：
// - 维护 n+1 个顶点；
// - 通过反射/扩张/收缩/整体收缩迭代改进最差顶点；
// - 用 rtol 判定收敛；
// - 通过 stop_requested_ 支持外部中断。

Simplex::Simplex(CostFunction fn) : cost_function_(std::move(fn)) {
    if (!cost_function_) {
        throw std::invalid_argument("Simplex requires a valid cost function");
    }
}

void Simplex::set_tolerance(double tol) {
    tolerance_ = tol > 0.0 ? tol : tolerance_;
}

void Simplex::set_max_iterations(int max_iterations) {
    max_iterations_ = max_iterations > 0 ? max_iterations : max_iterations_;
}

void Simplex::set_initial_step(double step) {
    initial_step_ = step > 0.0 ? step : initial_step_;
}

void Simplex::set_iteration_callback(IterationCallback cb) {
    iteration_callback_ = std::move(cb);
}

std::vector<double> Simplex::optimize(const std::vector<double>& initial_guess) {
    if (initial_guess.empty()) {
        throw std::invalid_argument("initial_guess must not be empty");
    }

    stop_requested_.store(false);
    iterations_ = 0;

    const int n = static_cast<int>(initial_guess.size());
    // 初始单纯形：第 0 个点是初值，其余点在各维加 initial_step。
    std::vector<std::vector<double>> p(static_cast<std::size_t>(n + 1), initial_guess);
    std::vector<double> y(static_cast<std::size_t>(n + 1), 0.0);

    for (int i = 0; i < n; ++i) {
        p[static_cast<std::size_t>(i + 1)][static_cast<std::size_t>(i)] += initial_step_;
    }

    for (int i = 0; i <= n; ++i) {
        y[static_cast<std::size_t>(i)] = cost_function_(p[static_cast<std::size_t>(i)]);
    }

    auto sum_point = [&](std::vector<double>& psum) {
        std::fill(psum.begin(), psum.end(), 0.0);
        for (int i = 0; i <= n; ++i) {
            for (int j = 0; j < n; ++j) {
                psum[static_cast<std::size_t>(j)] += p[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            }
        }
    };

    auto amotry = [&](int ihi, double fac, std::vector<double>& psum) {
        const double fac1 = (1.0 - fac) / static_cast<double>(n);
        const double fac2 = fac1 - fac;
        std::vector<double> ptry(static_cast<std::size_t>(n), 0.0);
        for (int j = 0; j < n; ++j) {
            ptry[static_cast<std::size_t>(j)] =
                psum[static_cast<std::size_t>(j)] * fac1 - p[static_cast<std::size_t>(ihi)][static_cast<std::size_t>(j)] * fac2;
        }
        const double ytry = cost_function_(ptry);
        if (ytry < y[static_cast<std::size_t>(ihi)]) {
            y[static_cast<std::size_t>(ihi)] = ytry;
            for (int j = 0; j < n; ++j) {
                psum[static_cast<std::size_t>(j)] +=
                    ptry[static_cast<std::size_t>(j)] - p[static_cast<std::size_t>(ihi)][static_cast<std::size_t>(j)];
                p[static_cast<std::size_t>(ihi)][static_cast<std::size_t>(j)] = ptry[static_cast<std::size_t>(j)];
            }
        }
        return ytry;
    };

    std::vector<double> psum(static_cast<std::size_t>(n), 0.0);
    sum_point(psum);

    while (!stop_requested_.load() && iterations_ < max_iterations_) {
        // 每轮寻找最优/最差/次差顶点，随后决定反射或收缩策略。
        int ilo = 0;
        int ihi = y[1] > y[0] ? 1 : 0;
        int inhi = y[1] > y[0] ? 0 : 1;

        for (int i = 0; i <= n; ++i) {
            if (y[static_cast<std::size_t>(i)] < y[static_cast<std::size_t>(ilo)]) {
                ilo = i;
            }
            if (y[static_cast<std::size_t>(i)] > y[static_cast<std::size_t>(ihi)]) {
                inhi = ihi;
                ihi = i;
            } else if (i != ihi && y[static_cast<std::size_t>(i)] > y[static_cast<std::size_t>(inhi)]) {
                inhi = i;
            }
        }

        const double rtol = 2.0 * std::fabs(y[static_cast<std::size_t>(ihi)] - y[static_cast<std::size_t>(ilo)]) /
                            (std::fabs(y[static_cast<std::size_t>(ihi)]) + std::fabs(y[static_cast<std::size_t>(ilo)]) +
                             std::numeric_limits<double>::epsilon());

        best_cost_ = y[static_cast<std::size_t>(ilo)];

        if (iteration_callback_) {
            iteration_callback_(iterations_, best_cost_, p[static_cast<std::size_t>(ilo)]);
        }

        if (rtol < tolerance_) {
            // 收敛：将最优顶点交换到 p[0] 统一返回。
            std::swap(y[0], y[static_cast<std::size_t>(ilo)]);
            std::swap(p[0], p[static_cast<std::size_t>(ilo)]);
            return p[0];
        }

        ++iterations_;

        double ytry = amotry(ihi, -1.0, psum);
        if (ytry <= y[static_cast<std::size_t>(ilo)]) {
            amotry(ihi, 2.0, psum);
        } else if (ytry >= y[static_cast<std::size_t>(inhi)]) {
            const double ysave = y[static_cast<std::size_t>(ihi)];
            ytry = amotry(ihi, 0.5, psum);
            if (ytry >= ysave) {
                for (int i = 0; i <= n; ++i) {
                    if (i != ilo) {
                        for (int j = 0; j < n; ++j) {
                            p[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                                0.5 * (p[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] +
                                       p[static_cast<std::size_t>(ilo)][static_cast<std::size_t>(j)]);
                        }
                        y[static_cast<std::size_t>(i)] = cost_function_(p[static_cast<std::size_t>(i)]);
                    }
                }
                sum_point(psum);
            }
        }
    }

    int best_idx = 0;
    for (int i = 1; i <= n; ++i) {
        if (y[static_cast<std::size_t>(i)] < y[static_cast<std::size_t>(best_idx)]) {
            best_idx = i;
        }
    }
    best_cost_ = y[static_cast<std::size_t>(best_idx)];
    return p[static_cast<std::size_t>(best_idx)];
}

void Simplex::stop() {
    stop_requested_.store(true);
}

int Simplex::iteration_count() const {
    return iterations_;
}

double Simplex::best_cost() const {
    return best_cost_;
}

}  // namespace registration


