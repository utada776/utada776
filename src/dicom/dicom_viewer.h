#pragma once

#include <string>
#include <vtkSmartPointer.h>

class vtkImageData;

namespace dicom_viewer {

// Validate that a directory exists and contains loadable DICOM slice data.
bool ValidateDicomDirectory(const std::string& directory, std::string& error_message);

// Open a DICOM folder in the VTK tri-planar/volume viewer.
bool ShowDicomVolume(const std::string& directory, std::string& error_message);

// Display an already loaded raw vtkImageData volume using the tri-planar viewer.
bool ShowRawVolume(const vtkSmartPointer<vtkImageData>& image,
                  const std::string& title,
                  std::string& error_message);

// Display an already loaded AVL .SCAN volume using the same VTK viewer path.
bool ShowScanVolume(const vtkSmartPointer<vtkImageData>& image,
                   const std::string& title,
                   std::string& error_message);

}  // namespace dicom_viewer
