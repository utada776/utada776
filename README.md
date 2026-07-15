# Imaging Studio

Imaging Studio is a C++17 CMake application for medical-image viewing, SCAN/raw volume loading, image registration, and cone-beam CT FDK reconstruction. The Windows build is validated with Visual Studio 2017 x64, wxWidgets, local VTK/DCMTK installs, and an optional local RTK/ITK build tree.

## Main Features

- wxWidgets desktop GUI with DICOM, RAW, SCAN, FDK reconstruction, registration, PPS demo, and Bean demo entry points.
- VTK-based DICOM/volume visualization.
- Optional DCMTK-backed DICOM export.
- RTK-backed FDK reconstruction when `external/rtk/build` and `external/itk/build` are present.
- Google Test unit tests for core, viewer validation, GUI style constants, and Bean style constants.

## Repository Layout

```text
.
├── .github/workflows/          # CI workflow
├── .vscode/                    # VS Code tasks/debug recommendations
├── build.ps1                   # Windows configure/build/test/run script
├── build.sh                    # Linux configure/build/test/run script
├── CMakeLists.txt              # Main CMake project
├── CMakePresets.json           # Optional CMake presets
├── include/                    # Public hello_core headers
├── src/                        # Application source
│   ├── FDK recon/              # FDK UI, RTK integration, DICOM export
│   ├── bean/                   # Bean demo
│   ├── dicom/                  # DICOM, RAW, and SCAN loading/viewing
│   ├── pps/                    # PPS demo
│   ├── registration/           # Registration UI and core algorithms
│   └── volume/                 # RAW/SCAN loader dialogs
├── tests/                      # Google Test targets
├── tools/                      # Developer reconstruction/SCAN inspection tools
└── external/                   # Dependency scripts/docs plus local dependency caches
```

The following paths are intentionally not tracked: `dicom data/`, `build-*`, `.deps/`, `external/*/build`, `external/*/install`, and `external/*/src`.

## Windows Build

Use the project script from PowerShell:

```powershell
cd "C:\code test"
.\build.ps1 -Configuration Release -NoRun -SkipOfflineBuild
```

The validated build command configures `build-vs2017-x64`, builds all targets when tests are enabled, and runs CTest. To build only the application without tests:

```powershell
.\build.ps1 -Configuration Release -NoRun -SkipTests -SkipOfflineBuild
```

To launch the GUI after a successful build, omit `-NoRun`:

```powershell
.\build.ps1 -Configuration Release -SkipOfflineBuild
```

Useful script parameters:

- `-Configuration Debug|Release`: choose build configuration. The current local RTK/ITK dependency cache is Release-oriented; use Release for full RTK builds unless Debug ITK/RTK libraries also exist.
- `-BuildDirectory <dir>`: override the default `build-vs2017-x64`.
- `-NoRun`: build/test only, do not launch the GUI.
- `-Clean`: pass `--clean-first` to the CMake build.
- `-SkipOfflineBuild`: fail instead of trying to build missing local VTK/DCMTK dependencies.
- `-SkipTests`: configure with `BUILD_TESTING=OFF` and build only `hello_cross_platform`.

## Tests

CTest is enabled by default in CMake. After configuring/building with tests:

```powershell
ctest --test-dir build-vs2017-x64 -C Release --output-on-failure
```

The currently validated local run passes 19 tests.

## Linux Build

Install system dependencies and run:

```bash
chmod +x build.sh
./build.sh --configuration Release --no-run
```

See [UBUNTU_GUI_SETUP.md](UBUNTU_GUI_SETUP.md) for package details and GUI/headless notes.

## External Dependencies

See [external/README.md](external/README.md) for the local dependency cache layout. The repository should keep dependency scripts and docs, but not generated dependency source/build/install directories.

## VS Code Tasks

The workspace tasks are intentionally small:

- `Configure x64`
- `Build x64 app`
- `Test x64`
- `Verify build.ps1`

Historical polling/debug tasks were removed to keep the workspace reproducible.
