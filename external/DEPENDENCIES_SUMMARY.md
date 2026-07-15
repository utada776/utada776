# Dependency Summary

This file records the dependency versions and local-cache expectations used by the current Windows build.

## VTK

| Item | Value |
| --- | --- |
| Version | 9.3.x local install |
| Purpose | DICOM/volume visualization through VTK rendering and image modules |
| Expected config | `external/vtk/install/share/vtk/vtk-config.cmake` |
| Cache policy | Do not commit `external/vtk/src`, `external/vtk/build`, or `external/vtk/install` |

## DCMTK

| Item | Value |
| --- | --- |
| Version | 3.7.0 local install |
| Purpose | Optional DICOM export/import support |
| Expected config | `external/dcmtk/install/cmake/DCMTKConfig.cmake` |
| Cache policy | Do not commit `external/dcmtk/src`, `external/dcmtk/build`, or `external/dcmtk/install` |

## RTK / ITK

| Item | Value |
| --- | --- |
| RTK | 2.7-style local build tree |
| ITK | 5.3-style local build tree |
| Purpose | Optional RTK-backed FDK reconstruction backend |
| Expected paths | `external/rtk/build`, `external/itk/build` |
| Build note | Current local cache is Release-oriented; Debug builds require matching Debug libraries |
| Cache policy | Do not commit RTK/ITK source, build, or install trees |

## wxWidgets

| Item | Value |
| --- | --- |
| Purpose | wxWidgets desktop GUI |
| Source | vcpkg/toolchain or system package config discovered by CMake |

## Google Test

| Item | Value |
| --- | --- |
| Version | v1.14.0 via CMake FetchContent |
| Cache | `.deps/` |
| Cache policy | Do not commit `.deps/` |

## Validation Command

```powershell
cd "C:\code test"
.\build.ps1 -Configuration Release -NoRun -SkipOfflineBuild
```

The current validation builds the app, developer tools, unit tests, and runs 19 CTest tests successfully.
