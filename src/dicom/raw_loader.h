#pragma once

#include <string>
#include <vector>
#include <vtkSmartPointer.h>

class vtkImageData;

namespace raw_volume {

// ============================================================================
// Raw volume file loader for binary float32 data
// ============================================================================

struct RawVolumeMetadata {
    std::string file_path;
    int dim_x = 0;
    int dim_y = 0;
    int dim_z = 0;
    float voxel_size_cm = 0.1f;
    // Source buffer linear order (fastest -> slowest), axis ids: 0=X, 1=Y, 2=Z.
    int source_order[3] = {2, 1, 0};
    // Output axis mapping (output axis -> source axis).
    int output_to_source_axis[3] = {0, 1, 2};
    bool auto_detect_layout = true;
};

// Load raw binary file into vtkImageData
// Returns nullptr on failure, error message in out_error
vtkSmartPointer<vtkImageData> LoadRawVolume(
    const RawVolumeMetadata& metadata,
    std::string& out_error);

// Validate that raw file size matches expected dimensions
// Returns true if file size == dim_x * dim_y * dim_z * sizeof(float)
bool ValidateRawFile(
    const std::string& file_path,
    int dim_x,
    int dim_y,
    int dim_z,
    std::string& out_error);

}  // namespace raw_volume
