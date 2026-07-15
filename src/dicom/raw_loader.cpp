#include "dicom/raw_loader.h"

#include <vtkImageData.h>
#include <vtkNew.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>

namespace {

bool IsAxisPermutation(const int order[3]) {
    bool seen[3] = {false, false, false};
    for (int i = 0; i < 3; ++i) {
        if (order[i] < 0 || order[i] > 2 || seen[order[i]]) {
            return false;
        }
        seen[order[i]] = true;
    }
    return true;
}

size_t SourceIndexFromCoord(
    const int source_dims[3],
    const int source_order[3],
    int sx,
    int sy,
    int sz) {
    const size_t dims[3] = {
        static_cast<size_t>(source_dims[0]),
        static_cast<size_t>(source_dims[1]),
        static_cast<size_t>(source_dims[2])};
    const size_t coord[3] = {
        static_cast<size_t>(sx),
        static_cast<size_t>(sy),
        static_cast<size_t>(sz)};

    const int fast = source_order[0];
    const int mid = source_order[1];
    const int slow = source_order[2];
    return coord[fast] + dims[fast] * (coord[mid] + dims[mid] * coord[slow]);
}

double ComputeContinuityScore(
    const std::vector<float>& buffer,
    const int source_dims[3],
    const int source_order[3],
    const int output_to_source_axis[3]) {
    const int out_dims[3] = {
        source_dims[output_to_source_axis[0]],
        source_dims[output_to_source_axis[1]],
        source_dims[output_to_source_axis[2]]};

    if (out_dims[0] <= 1 && out_dims[1] <= 1 && out_dims[2] <= 1) {
        return std::numeric_limits<double>::infinity();
    }

    std::mt19937 rng(1234567u);
    std::uniform_int_distribution<int> dx(0, std::max(0, out_dims[0] - 1));
    std::uniform_int_distribution<int> dy(0, std::max(0, out_dims[1] - 1));
    std::uniform_int_distribution<int> dz(0, std::max(0, out_dims[2] - 1));

    double sum_abs_diff = 0.0;
    int diff_count = 0;

    const int sample_count = 4000;
    for (int i = 0; i < sample_count; ++i) {
        const int ox = dx(rng);
        const int oy = dy(rng);
        const int oz = dz(rng);

        int src0[3] = {0, 0, 0};
        src0[output_to_source_axis[0]] = ox;
        src0[output_to_source_axis[1]] = oy;
        src0[output_to_source_axis[2]] = oz;

        const size_t idx0 = SourceIndexFromCoord(source_dims, source_order, src0[0], src0[1], src0[2]);
        const float v0 = buffer[idx0];

        if (ox + 1 < out_dims[0]) {
            int src1[3] = {0, 0, 0};
            src1[output_to_source_axis[0]] = ox + 1;
            src1[output_to_source_axis[1]] = oy;
            src1[output_to_source_axis[2]] = oz;
            const size_t idx1 = SourceIndexFromCoord(source_dims, source_order, src1[0], src1[1], src1[2]);
            sum_abs_diff += std::abs(static_cast<double>(v0 - buffer[idx1]));
            ++diff_count;
        }
        if (oy + 1 < out_dims[1]) {
            int src1[3] = {0, 0, 0};
            src1[output_to_source_axis[0]] = ox;
            src1[output_to_source_axis[1]] = oy + 1;
            src1[output_to_source_axis[2]] = oz;
            const size_t idx1 = SourceIndexFromCoord(source_dims, source_order, src1[0], src1[1], src1[2]);
            sum_abs_diff += std::abs(static_cast<double>(v0 - buffer[idx1]));
            ++diff_count;
        }
        if (oz + 1 < out_dims[2]) {
            int src1[3] = {0, 0, 0};
            src1[output_to_source_axis[0]] = ox;
            src1[output_to_source_axis[1]] = oy;
            src1[output_to_source_axis[2]] = oz + 1;
            const size_t idx1 = SourceIndexFromCoord(source_dims, source_order, src1[0], src1[1], src1[2]);
            sum_abs_diff += std::abs(static_cast<double>(v0 - buffer[idx1]));
            ++diff_count;
        }
    }

    if (diff_count == 0) {
        return std::numeric_limits<double>::infinity();
    }

    return sum_abs_diff / static_cast<double>(diff_count);
}

}  // namespace

namespace raw_volume {

bool ValidateRawFile(
    const std::string& file_path,
    int dim_x,
    int dim_y,
    int dim_z,
    std::string& out_error) {
    namespace fs = std::filesystem;

    if (dim_x <= 0 || dim_y <= 0 || dim_z <= 0) {
        out_error = "Invalid dimensions: " + std::to_string(dim_x) + " x " +
                    std::to_string(dim_y) + " x " + std::to_string(dim_z);
        return false;
    }

    std::error_code ec;
    if (!fs::exists(file_path, ec)) {
        out_error = "File not found: " + file_path;
        return false;
    }

    const auto file_size = fs::file_size(file_path, ec);
    if (ec) {
        out_error = "Failed to get file size: " + ec.message();
        return false;
    }

    const size_t expected_size = static_cast<size_t>(dim_x) * 
                                 static_cast<size_t>(dim_y) * 
                                 static_cast<size_t>(dim_z) * 
                                 sizeof(float);

    if (file_size != expected_size) {
        std::ostringstream ss;
        ss << "File size mismatch.\n"
           << "Expected: " << expected_size << " bytes ("
           << dim_x << " x " << dim_y << " x " << dim_z << " x 4)\n"
           << "Actual: " << file_size << " bytes";
        out_error = ss.str();
        return false;
    }

    return true;
}

vtkSmartPointer<vtkImageData> LoadRawVolume(
    const RawVolumeMetadata& metadata,
    std::string& out_error) {
    
    // Validate metadata
    if (!ValidateRawFile(metadata.file_path, metadata.dim_x, metadata.dim_y, 
                        metadata.dim_z, out_error)) {
        return nullptr;
    }

    // Read file into buffer
    std::ifstream file(metadata.file_path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "Failed to open file: " + metadata.file_path;
        return nullptr;
    }

    const size_t num_voxels = static_cast<size_t>(metadata.dim_x) * 
                              static_cast<size_t>(metadata.dim_y) * 
                              static_cast<size_t>(metadata.dim_z);
    
    std::vector<float> buffer(num_voxels);
    file.read(reinterpret_cast<char*>(buffer.data()), 
              static_cast<std::streamsize>(num_voxels * sizeof(float)));

    if (!file || file.gcount() != static_cast<std::streamsize>(num_voxels * sizeof(float))) {
        out_error = "Failed to read complete file data";
        return nullptr;
    }

    file.close();

    const int source_dims[3] = {metadata.dim_x, metadata.dim_y, metadata.dim_z};
    int chosen_source_order[3] = {
        metadata.source_order[0],
        metadata.source_order[1],
        metadata.source_order[2]};
    int chosen_output_to_source_axis[3] = {
        metadata.output_to_source_axis[0],
        metadata.output_to_source_axis[1],
        metadata.output_to_source_axis[2]};

    if (!metadata.auto_detect_layout) {
        if (!IsAxisPermutation(chosen_source_order)) {
            out_error = "Invalid source buffer order (must be a permutation of 0,1,2)";
            return nullptr;
        }
        if (!IsAxisPermutation(chosen_output_to_source_axis)) {
            out_error = "Invalid axis mapping (must be a permutation of 0,1,2)";
            return nullptr;
        }
    } else {
        const int perms[6][3] = {
            {0, 1, 2},
            {0, 2, 1},
            {1, 0, 2},
            {1, 2, 0},
            {2, 0, 1},
            {2, 1, 0}};

        double best_score = std::numeric_limits<double>::infinity();
        int best_source_order[3] = {2, 1, 0};
        int best_output_map[3] = {0, 1, 2};

        for (const auto& source_order : perms) {
            for (const auto& output_map : perms) {
                const double score = ComputeContinuityScore(
                    buffer,
                    source_dims,
                    source_order,
                    output_map);
                if (score < best_score) {
                    best_score = score;
                    best_source_order[0] = source_order[0];
                    best_source_order[1] = source_order[1];
                    best_source_order[2] = source_order[2];
                    best_output_map[0] = output_map[0];
                    best_output_map[1] = output_map[1];
                    best_output_map[2] = output_map[2];
                }
            }
        }

        chosen_source_order[0] = best_source_order[0];
        chosen_source_order[1] = best_source_order[1];
        chosen_source_order[2] = best_source_order[2];
        chosen_output_to_source_axis[0] = best_output_map[0];
        chosen_output_to_source_axis[1] = best_output_map[1];
        chosen_output_to_source_axis[2] = best_output_map[2];
    }

    const int out_dims[3] = {
        source_dims[chosen_output_to_source_axis[0]],
        source_dims[chosen_output_to_source_axis[1]],
        source_dims[chosen_output_to_source_axis[2]]};

    // Create vtkImageData
    vtkNew<vtkImageData> image;
    image->SetDimensions(out_dims[0], out_dims[1], out_dims[2]);
    image->SetSpacing(metadata.voxel_size_cm, metadata.voxel_size_cm, metadata.voxel_size_cm);
    image->AllocateScalars(VTK_FLOAT, 1);

    // Map source buffer order and axis mapping into VTK x-fastest layout.
    auto* dst = static_cast<float*>(image->GetScalarPointer());
    for (int oz = 0; oz < out_dims[2]; ++oz) {
        for (int oy = 0; oy < out_dims[1]; ++oy) {
            for (int ox = 0; ox < out_dims[0]; ++ox) {
                int src_coord[3] = {0, 0, 0};
                src_coord[chosen_output_to_source_axis[0]] = ox;
                src_coord[chosen_output_to_source_axis[1]] = oy;
                src_coord[chosen_output_to_source_axis[2]] = oz;

                const size_t src_idx = SourceIndexFromCoord(
                    source_dims,
                    chosen_source_order,
                    src_coord[0],
                    src_coord[1],
                    src_coord[2]);
                const size_t dst_idx =
                    (static_cast<size_t>(oz) * static_cast<size_t>(out_dims[1]) + static_cast<size_t>(oy)) *
                    static_cast<size_t>(out_dims[0]) +
                    static_cast<size_t>(ox);
                dst[dst_idx] = buffer[src_idx];
            }
        }
    }

    return image;
}

}  // namespace raw_volume
