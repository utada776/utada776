#pragma once
// ============================================================================
// dicom_exporter.h
//
// DCMTK-based DICOM CT series export for FDK reconstruction volumes.
// Each axial slice is written as a separate .dcm file.
//
// Build requirement:
//   CMake defines FDK_HAS_DCMTK when DCMTK is found. The validated Windows
//   path uses external/dcmtk/install; vcpkg dcmtk:x64-windows is also usable
//   when configuring without local dependencies.
// ============================================================================

#include "FDK recon/fdk_types.h"
#include <string>

namespace dicom_export {

/// Export a reconstructed FDK volume as a DICOM CT series.
///
/// @param vol      The reconstructed 3-D volume (raw reconstruction units).
/// @param params   FDK parameters; voxel_size_cm, scale_out and offset_out
///                 are used to convert raw values to HU.
/// @param out_dir  Output directory. Created if it does not exist.
/// @param error    Error description on failure.
/// @return true on success.
bool ExportFdkVolume(const fdk::Volume3D&  vol,
                     const fdk::FdkParams& params,
                     const std::string&    out_dir,
                     std::string&          error);

} // namespace dicom_export
