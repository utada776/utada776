# Windows GUI Setup

The GUI uses wxWidgets for the desktop shell, VTK for image/volume rendering, DCMTK for optional DICOM export, and RTK/ITK for FDK reconstruction when the local RTK backend is available.

## Dependencies

| Dependency | Purpose | Expected Source |
| --- | --- | --- |
| wxWidgets | GUI framework | vcpkg/toolchain or system package config |
| VTK | DICOM and volume visualization | `external/vtk/install` |
| DCMTK | Optional DICOM export | `external/dcmtk/install` |
| RTK/ITK | Optional FDK backend | `external/rtk/build`, `external/itk/build` |
| Google Test | Unit tests | `.deps/` via CMake FetchContent |

Generated dependency directories are local caches and should not be committed.

## Build And Launch

Validate the project without starting the GUI:

```powershell
cd "C:\code test"
.\build.ps1 -Configuration Release -NoRun -SkipOfflineBuild
```

Launch after a successful build:

```powershell
.\build.ps1 -Configuration Release -SkipOfflineBuild
```

Run the executable directly if the required DLL directories are already on `PATH`:

```powershell
.\build-vs2017-x64\Release\hello_cross_platform.exe --gui
```

## GUI Entry Points

- `Load 3D DICOM Folder`: validate and display DICOM image folders.
- `3D Reconstruction`: open the FDK reconstruction dialog.
- `Load Raw Volume`: load float/raw binary volume data with user-provided metadata.
- `Load Scan Volume`: load AVL `.SCAN` volumes.
- `3D Registration`: open the registration workflow.
- `Open PPS Demo`: show the PPS movement demo.
- `Open Bean Game`: show the Bean keyboard demo.

## Tests

The GUI-adjacent tests are built and run by the default Release validation command. They currently cover GUI style constants, Bean style constants, and DICOM directory validation. They do not launch an interactive GUI window.

## Notes

- Prefer `Release` for RTK-enabled local builds unless matching Debug ITK/RTK libraries exist.
- `build.ps1` prefixes `PATH` with local VTK/DCMTK and vcpkg binary directories before launching the application.
- The VS Code task `Verify build.ps1` runs the same non-launching validation path.
