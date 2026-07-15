# Windows Build And Run Guide

This project is validated on Windows with Visual Studio 2017 x64, CMake, wxWidgets, local VTK/DCMTK installs, and an optional local RTK/ITK build tree.

## Requirements

- Windows 10/11 x64
- Visual Studio 2017 or newer with the C++ workload
- CMake 3.16 or newer
- PowerShell 5+ or PowerShell 7+
- Local dependencies under `external/`:
  - `external/vtk/install/share/vtk/vtk-config.cmake`
  - `external/dcmtk/install/cmake/DCMTKConfig.cmake`
  - optional RTK/ITK build trees under `external/rtk/build` and `external/itk/build`

## Recommended Validation Command

```powershell
cd "C:\code test"
.\build.ps1 -Configuration Release -NoRun -SkipOfflineBuild
```

This command:

1. Checks local VTK and DCMTK installs.
2. Configures `build-vs2017-x64` with Visual Studio 2017 x64.
3. Enables unit tests.
4. Builds all targets needed by CTest.
5. Runs `ctest -C Release --output-on-failure`.

## Common Build Commands

Build and run tests without launching the GUI:

```powershell
.\build.ps1 -Configuration Release -NoRun -SkipOfflineBuild
```

Build only the application target:

```powershell
.\build.ps1 -Configuration Release -NoRun -SkipTests -SkipOfflineBuild
```

Clean rebuild:

```powershell
.\build.ps1 -Configuration Release -NoRun -Clean -SkipOfflineBuild
```

Build and launch the GUI:

```powershell
.\build.ps1 -Configuration Release -SkipOfflineBuild
```

## Script Parameters

- `-Configuration Debug|Release`: selects the build configuration. Use `Release` for the current local RTK/ITK cache unless Debug ITK/RTK libraries have been built.
- `-BuildDirectory <dir>`: selects the CMake build directory. Default is `build-vs2017-x64`.
- `-NoRun`: skips launching `hello_cross_platform.exe` after build/test.
- `-Clean`: requests a clean CMake build.
- `-UseVcpkg`: retained as a compatibility switch; the current script still configures local dependencies by default and adds the vcpkg toolchain when available.
- `-SkipOfflineBuild`: fails early if local VTK/DCMTK installs are missing.
- `-SkipTests`: configures `BUILD_TESTING=OFF` and builds only `hello_cross_platform`.

## Manual CMake Commands

```powershell
cmake -S . -B build-vs2017-x64 -G "Visual Studio 15 2017" -A x64 -DUSE_LOCAL_DEPENDENCIES=ON -DBUILD_TESTING=ON
cmake --build build-vs2017-x64 --config Release --target ALL_BUILD --parallel 4
ctest --test-dir build-vs2017-x64 -C Release --output-on-failure
```

## Output

Application executable:

```text
build-vs2017-x64\Release\hello_cross_platform.exe
```

## Troubleshooting

### CMake Not Found

Install CMake and ensure `cmake.exe` is on `PATH`, or install it at `C:\Program Files\CMake\bin\cmake.exe`.

### Visual Studio Environment Not Found

Install Visual Studio or Build Tools with the C++ workload. The script searches common `vcvars64.bat` locations for VS 2022, VS 2019, and VS 2017.

### Missing Local VTK/DCMTK

Run the dependency build script if the source packages are available locally:

```powershell
cd external
.\BUILD_ALL_OFFLINE.ps1
```

Or rebuild/provide the local install directories listed in the requirements section.

### Debug Build Cannot Link ITK/RTK

The current local RTK/ITK cache is Release-oriented. A Debug build can fail while looking for Debug ITK libraries. Use `-Configuration Release`, or build matching Debug ITK/RTK libraries.

### Runtime DLL Missing

Use `build.ps1` so the runtime `PATH` includes local VTK/DCMTK and vcpkg binary directories. To diagnose without launching the GUI, pass `-NoRun`.
