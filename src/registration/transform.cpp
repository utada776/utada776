#include "registration/transform.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace registration {

// Transform 采用行主序存储 4x4 矩阵，便于与现有代码中的 get/set(row,col) 对齐。

namespace {

constexpr float kPi = 3.14159265358979323846f;

float deg_to_rad(float deg) {
    return deg * kPi / 180.0f;
}

}  // namespace

Transform::Transform() {
    make_identity();
}

void Transform::clear() {
    m_.fill(0.0f);
}

void Transform::make_identity() {
    clear();
    set(0, 0, 1.0f);
    set(1, 1, 1.0f);
    set(2, 2, 1.0f);
    set(3, 3, 1.0f);
}

void Transform::make_translation(float tx, float ty, float tz) {
    make_identity();
    set(0, 3, tx);
    set(1, 3, ty);
    set(2, 3, tz);
}

void Transform::make_rotation(float rx_deg, float ry_deg, float rz_deg) {
    // 欧拉角按 X/Y/Z 生成基础旋转矩阵，最终组合顺序为 Rz * Ry * Rx。
    const float ax = deg_to_rad(rx_deg);
    const float ay = deg_to_rad(ry_deg);
    const float az = deg_to_rad(rz_deg);

    const float cx = std::cos(ax);
    const float sx = std::sin(ax);
    const float cy = std::cos(ay);
    const float sy = std::sin(ay);
    const float cz = std::cos(az);
    const float sz = std::sin(az);

    Transform rx;
    rx.make_identity();
    rx.set(1, 1, cx);
    rx.set(1, 2, -sx);
    rx.set(2, 1, sx);
    rx.set(2, 2, cx);

    Transform ry;
    ry.make_identity();
    ry.set(0, 0, cy);
    ry.set(0, 2, sy);
    ry.set(2, 0, -sy);
    ry.set(2, 2, cy);

    Transform rz;
    rz.make_identity();
    rz.set(0, 0, cz);
    rz.set(0, 1, -sz);
    rz.set(1, 0, sz);
    rz.set(1, 1, cz);

    *this = multiply(rz, multiply(ry, rx));
}

void Transform::make_scaling(float sx, float sy, float sz) {
    make_identity();
    set(0, 0, sx);
    set(1, 1, sy);
    set(2, 2, sz);
}

void Transform::make_translation_after_rotation(float tx, float ty, float tz, float rx_deg, float ry_deg,
                                                float rz_deg) {
    Transform r;
    r.make_rotation(rx_deg, ry_deg, rz_deg);

    Transform t;
    t.make_translation(tx, ty, tz);

    *this = multiply(t, r);
}

void Transform::get_translation(float& tx, float& ty, float& tz) const {
    tx = get(0, 3);
    ty = get(1, 3);
    tz = get(2, 3);
}

void Transform::get_scaling(float& sx, float& sy, float& sz) const {
    sx = std::sqrt(get(0, 0) * get(0, 0) + get(1, 0) * get(1, 0) + get(2, 0) * get(2, 0));
    sy = std::sqrt(get(0, 1) * get(0, 1) + get(1, 1) * get(1, 1) + get(2, 1) * get(2, 1));
    sz = std::sqrt(get(0, 2) * get(0, 2) + get(1, 2) * get(1, 2) + get(2, 2) * get(2, 2));
}

void Transform::transpose() {
    for (int r = 0; r < 4; ++r) {
        for (int c = r + 1; c < 4; ++c) {
            const float tmp = get(r, c);
            set(r, c, get(c, r));
            set(c, r, tmp);
        }
    }
}

bool Transform::invert() {
    // 高斯-约旦消元求逆；若主元过小则视为不可逆。
    float a[4][8] = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = get(r, c);
        }
        a[r][r + 4] = 1.0f;
    }

    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-8f) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[col][c], a[pivot][c]);
            }
        }

        const float diag = a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] /= diag;
        }

        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float f = a[r][col];
            for (int c = 0; c < 8; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            set(r, c, a[r][c + 4]);
        }
    }
    return true;
}

void Transform::pre_multiply(const Transform& pre) {
    *this = multiply(pre, *this);
}

void Transform::post_multiply(const Transform& post) {
    *this = multiply(*this, post);
}

bool Transform::equals(const Transform& rhs, float tolerance) const {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(m_[i] - rhs.m_[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

float Transform::get(int row, int col) const {
    return m_.at(static_cast<std::size_t>(row) * 4 + static_cast<std::size_t>(col));
}

void Transform::set(int row, int col, float value) {
    m_.at(static_cast<std::size_t>(row) * 4 + static_cast<std::size_t>(col)) = value;
}

std::string Transform::as_string(int decimals) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (r != 0 || c != 0) {
                oss << ' ';
            }
            oss << get(r, c);
        }
    }
    return oss.str();
}

Transform Transform::multiply(const Transform& a, const Transform& b) {
    Transform out;
    out.clear();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) {
                v += a.get(r, k) * b.get(k, c);
            }
            out.set(r, c, v);
        }
    }
    return out;
}

}  // namespace registration


