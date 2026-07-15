#pragma once

#include <array>
#include <string>

namespace registration {

// ============================================================================
// Transform  ── 4×4 齐次变换矩阵
//
// 用于表示三维空间中的刚体变换（平移 + 旋转）及仿射变换（含缩放）。
// 存储格式：列主序 float[16]，索引 m_[row*4+col]。
//
// 常用操作：
//   make_translation / make_rotation / make_scaling  ── 基本变换构造
//   pre_multiply(T)   ── 左乘：this = T × this
//   post_multiply(T)  ── 右乘：this = this × T
//   invert()          ── 原地求逆（失败返回 false，矩阵不变）
//
// 坐标约定：列向量，变换作用方式为 p' = M × p。
// ============================================================================

class Transform {
public:
    Transform();

    void clear();
    void make_identity();
    void make_translation(float tx, float ty, float tz);   ///< 构造纯平移矩阵
    void make_rotation(float rx_deg, float ry_deg, float rz_deg); ///< 构造欧拉角旋转（ZYX 顺序，度）
    void make_scaling(float sx, float sy, float sz);       ///< 构造各向异性缩放矩阵
    void make_translation_after_rotation(float tx, float ty, float tz, float rx_deg, float ry_deg,
                                         float rz_deg);

    void get_translation(float& tx, float& ty, float& tz) const; ///< 提取平移分量
    void get_scaling(float& sx, float& sy, float& sz) const;     ///< 提取缩放分量（近似）

    void transpose();  ///< 原地转置（仅旋转矩阵时等价于求逆）
    bool invert();     ///< 原地求逆，返回 false 表示矩阵奇异

    void pre_multiply(const Transform& pre);   ///< 左乘：this = pre × this
    void post_multiply(const Transform& post); ///< 右乘：this = this × post

    bool equals(const Transform& rhs, float tolerance = 1e-5f) const;

    float get(int row, int col) const;         ///< 读取矩阵元素（行列索引，0-based）
    void set(int row, int col, float value);   ///< 设置矩阵元素

    std::string as_string(int decimals = 6) const;

private:
    std::array<float, 16> m_{};

    static Transform multiply(const Transform& a, const Transform& b);
};

}  // namespace registration


