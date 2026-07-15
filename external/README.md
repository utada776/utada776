# External Dependency Cache

The `external/` directory holds helper scripts, dependency notes, and local dependency caches. Only lightweight scripts and documentation should be committed. Downloaded source trees, build directories, and install trees are generated local artifacts.

## Tracked Content

```text
external/
├── BUILD_ALL_OFFLINE.ps1
├── DEPENDENCIES_SUMMARY.md
├── README.md
├── dcmtk/INSTALL_DCMTK_OFFLINE.md
└── ... optional dependency notes/scripts
```

## Ignored Local Caches

```text
external/*/src/
external/*/build/
external/*/install/
```

These folders can be very large and may contain third-party source code, DLLs, EXEs, generated CMake files, and local machine paths. Keep them out of git.

## Expected Windows Install Locations

`build.ps1` checks these local installs by default:

```text
external/vtk/install/share/vtk/vtk-config.cmake
external/dcmtk/install/cmake/DCMTKConfig.cmake
```

The main CMake project also detects an optional RTK backend from:

```text
external/rtk/build
external/itk/build
```

The current local RTK/ITK cache is Release-oriented. Use `Release` for full RTK builds unless Debug variants are also built.

## Building Local Dependencies

If local VTK/DCMTK installs are missing and the source packages are available, run:

```powershell
cd "C:\code test\external"
.\BUILD_ALL_OFFLINE.ps1
```

Then validate the main project:

```powershell
cd "C:\code test"
.\build.ps1 -Configuration Release -NoRun -SkipOfflineBuild
```

## Version Summary

See [DEPENDENCIES_SUMMARY.md](DEPENDENCIES_SUMMARY.md) for versions and build notes.
