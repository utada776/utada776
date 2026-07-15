#pragma once

#include "FDK recon/fdk_types.h"

#include <string>

namespace fdk_dicom_exporter {
bool ExportVolume(const fdk::Volume3D& volume, const fdk::FdkParams& params, const std::string& out_dir, std::string& error);
}
