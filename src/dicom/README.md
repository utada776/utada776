# DICOM, RAW, And SCAN Module

This module contains the application-facing image loading, viewing, and optional DICOM export code.

## Files

- `dicom_viewer.h/.cpp`: validates DICOM folders and opens VTK-based viewers for DICOM, RAW, and SCAN volumes.
- `dicom_exporter.h/.cpp`: exports reconstructed FDK volumes as DICOM CT series when DCMTK is available.
- `raw_loader.h/.cpp`: loads dense little-endian float32 raw volume buffers into `vtkImageData`.
- `scan_loader.h/.cpp`: loads AVL `.SCAN` / AVS Field XDR volumes into `vtkImageData` for display.

## Dependency Boundaries

VTK is required for viewer and volume data construction. DCMTK is optional: CMake defines `FDK_HAS_DCMTK` only when DCMTK is found. Without DCMTK, DICOM export remains linkable and returns a clear runtime error message.

The validated Windows build uses local dependencies under `external/vtk/install` and `external/dcmtk/install`. Generated dependency source, build, install, image, and DICOM data directories are intentionally ignored by git.

## Tests

`tests/dicom_viewer_test.cpp` covers DICOM directory validation failure paths. Viewer functions are intentionally not launched from unit tests because they create interactive VTK/wxWidgets windows.
