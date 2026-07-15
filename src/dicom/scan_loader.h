#pragma once

#include <string>
#include <vtkSmartPointer.h>

class vtkImageData;

namespace scan_volume {

// ============================================================================
// AVS Field / XDR SCAN file loader
// SCAN file format: ASCII header (key=value, terminated by 0x0C 0x0C)
//                  followed by big-endian int16 (xdr_short) voxel data
// ============================================================================

struct ScanVolumeMetadata {
    std::string file_path;  // Path to the .SCAN file
};

// Load AVL .SCAN (AVS Field / XDR) file into vtkImageData.
// Returns nullptr on failure; error message written to out_error.
vtkSmartPointer<vtkImageData> LoadScanVolume(
    const ScanVolumeMetadata& metadata,
    std::string& out_error);

}  // namespace scan_volume
