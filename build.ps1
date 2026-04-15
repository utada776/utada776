param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$BuildDirectory = ''
)

$ErrorActionPreference = 'Stop'

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $defaultPath = 'C:\Program Files\CMake\bin\cmake.exe'
    if (Test-Path $defaultPath) {
        return $defaultPath
    }

    throw 'CMake was not found. Install CMake or add it to PATH.'
}

function Find-Ctest {
    $command = Get-Command ctest -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $defaultPath = 'C:\Program Files\CMake\bin\ctest.exe'
    if (Test-Path $defaultPath) {
        return $defaultPath
    }

    throw 'CTest was not found. Install CMake or add it to PATH.'
}

function Find-VcVars {
    $candidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat'
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw 'No Visual Studio C++ developer environment was found.'
}

function Check-wxWidgets {
    # This is optional if using vcpkg; if found, great; if not, suggest installation
    Write-Host "Note: This project now requires wxWidgets 3.0 for GUI support." -ForegroundColor Cyan
    Write-Host "Install it with: vcpkg install wxwidgets:x64-windows" -ForegroundColor Cyan
}

$cmake = Find-CMake
$ctest = Find-Ctest
$vcvars = Find-VcVars

Check-wxWidgets

$sourceDir = (Resolve-Path $PSScriptRoot).Path

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = 'build-vs2017-x64'
}

$buildDir = Join-Path $sourceDir $BuildDirectory
$executable = Join-Path $buildDir "$Configuration\hello_cross_platform.exe"

$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    $defaultVcpkgRoot = 'C:\tools\vcpkg'
    if (Test-Path $defaultVcpkgRoot) {
        $vcpkgRoot = $defaultVcpkgRoot
        Write-Host "Detected vcpkg at: $vcpkgRoot"
    }
}

$toolchainArg = ""
if ($vcpkgRoot) {
    $candidateToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path $candidateToolchain) {
        $triplet = 'x64-windows'
        $toolchainArg = " -DCMAKE_TOOLCHAIN_FILE=`"$candidateToolchain`" -DVCPKG_TARGET_TRIPLET=$triplet"
        Write-Host "Using vcpkg toolchain: $candidateToolchain"
        Write-Host "Using vcpkg triplet: $triplet"

        $cacheFile = Join-Path $buildDir 'CMakeCache.txt'
        if (Test-Path $cacheFile) {
            $cacheContent = Get-Content $cacheFile -Raw
            if ($cacheContent -notmatch 'CMAKE_TOOLCHAIN_FILE:FILEPATH=') {
                Write-Host "Removing stale CMake cache in $buildDir so vcpkg toolchain can be applied." -ForegroundColor Yellow
                Remove-Item -Path $buildDir -Recurse -Force
            }
        }
    }
}

$cmd = @(
    "call `"$vcvars`" >nul 2>&1",
    "`"$cmake`" -S `"$sourceDir`" -B `"$buildDir`" -A x64$toolchainArg",
    "`"$cmake`" --build `"$buildDir`" --config $Configuration",
    "`"$ctest`" --test-dir `"$buildDir`" -C $Configuration --output-on-failure",
    "`"$executable`" --gui"
) -join ' && '

Write-Host "Using CMake: $cmake"
Write-Host "Using vcvars: $vcvars"
Write-Host "Platform: x64"
Write-Host "Build directory: $buildDir"

cmd /c $cmd

if ($LASTEXITCODE -ne 0) {
    throw "Build pipeline failed with exit code $LASTEXITCODE. Delete stale cache under '$buildDir' if needed."
}