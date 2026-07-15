#pragma once

#include <string>
#include <vtkSmartPointer.h>

class vtkImageData;

namespace dicom_viewer {

bool ValidateDicomDirectory(const std::string& directory, std::string& error_message);

bool ShowDicomVolume(const std::string& directory, std::string& error_message);

// Display a raw vtkImageData volume using the tri-planar viewer
bool ShowRawVolume(const vtkSmartPointer<vtkImageData>& image,
                  const std::string& title,
                  std::string& error_message);

bool ShowScanVolume(const vtkSmartPointer<vtkImageData>& image,
                   const std::string& title,
                   std::string& error_message);

}  // namespace dicom_viewer
