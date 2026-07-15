#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace registration {

// ============================================================================
// registration_types.h  ── 配准模块共享数据类型
//
// 本文件定义配准内核使用的最小数据结构：
//   Image3D  ── 三维体素图像容器（固定尺寸 + 连续 float 数组）
//   Dot3f    ── 三维浮点坐标点（用于点集配准）
//   Size3i   ── 整型三维尺寸
//
// 注意：Image3D 有两种语义复用：
//   1. 常规体素图像（CT / CBCT / 分割图）
//   2. 点列表结构（size.z == 1，voxels 存储 [x0,y0,z0, x1,y1,z1, ...] 序列）
//      在 Chamfer 配准中使用；调用方需自行约定哪种用法。
// ============================================================================

struct Size3i {
    int x = 0;
    int y = 0;
    int z = 0;
};

// 迁移后配准逻辑使用的最小体数据结构，像素语义由调用方定义。
struct Image3D {
    Size3i size;
    std::vector<float> voxels;

    std::size_t voxel_count() const {
        return static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) *
               static_cast<std::size_t>(size.z);
    }

    bool valid() const {
        return size.x > 0 && size.y > 0 && size.z > 0 && voxels.size() == voxel_count();
    }

    float at(int x, int y, int z) const {
        const std::size_t idx = static_cast<std::size_t>(z) * static_cast<std::size_t>(size.x) *
                                    static_cast<std::size_t>(size.y) +
                                static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) +
                                static_cast<std::size_t>(x);
        return voxels.at(idx);
    }
};

struct Dot3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

}  // namespace registration


