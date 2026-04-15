#pragma once

#include <string>

namespace dicom_viewer {

bool ShowDicomVolume(const std::string& directory, std::string& error_message);

}  // namespace dicom_viewer
