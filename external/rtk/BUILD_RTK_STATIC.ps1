# BUILD_RTK_STATIC.ps1
# Download and build ITK + RTK as static local dependencies without using git.
# Usage from repo root:
#   powershell -ExecutionPolicy Bypass -File external\rtk\BUILD_RTK_STATIC.ps1

param(
    [string]$ItkVersion = "v5.3.0",
    [string]$RtkVersion = "v2.7.0",
    [int]$Parallel = 4,
    [switch]$SkipDownload = $false,
    [switch]$ForceClean = $false
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExternalDir = Split-Path -Parent $ScriptDir
$ItkRoot = Join-Path $ExternalDir "itk"
$RtkRoot = $ScriptDir

function Log([string]$Message) {
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] $Message"
}

function Test-RequiredCommand([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name was not found in PATH."
    }
}

function Save-AndExpandSourceArchive([string]$Url, [string]$Archive, [string]$Destination, [string]$FinalSourceDir) {
    if ((Test-Path $FinalSourceDir) -and (Test-Path (Join-Path $FinalSourceDir "CMakeLists.txt"))) {
        Log "Source already exists: $FinalSourceDir"
        return
    }
    if (Test-Path $FinalSourceDir) {
        Log "Removing incomplete source directory: $FinalSourceDir"
        Remove-Item -Recurse -Force $FinalSourceDir
    }
    if ($SkipDownload) {
        throw "Missing source directory and -SkipDownload was specified: $FinalSourceDir"
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Log "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Archive -UseBasicParsing -TimeoutSec 900
    Log "Extracting $Archive"
    tar -xzf $Archive -C $Destination
    if ($LASTEXITCODE -ne 0) { throw "tar failed for $Archive" }

    $extracted = Get-ChildItem -Path $Destination -Directory | Where-Object { $_.Name -ne "src" -and $_.Name -ne "build" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $extracted) { throw "Could not locate extracted source under $Destination" }
    if (Test-Path $FinalSourceDir) { Remove-Item -Recurse -Force $FinalSourceDir }
    $moved = $false
    for ($attempt = 1; $attempt -le 5 -and -not $moved; $attempt++) {
        try {
            Move-Item -Path $extracted.FullName -Destination $FinalSourceDir -ErrorAction Stop
            $moved = $true
        } catch {
            Log "Move attempt $attempt failed, retrying... $_"
            Start-Sleep -Seconds 2
        }
    }
    if (-not $moved) {
        throw "Could not move extracted source to $FinalSourceDir"
    }
    Remove-Item $Archive -ErrorAction SilentlyContinue
}

function Ensure-Itk-Rtk-Compatibility([string]$ItkSourceDir) {
    $macroPath = Join-Path $ItkSourceDir "Modules/Core/Common/include/itkMacro.h"
    if (-not (Test-Path $macroPath)) { throw "Cannot find ITK macro header: $macroPath" }
    $macroText = Get-Content -Raw $macroPath
    if ($macroText.Contains("itkOverrideGetNameOfClassMacro")) { return }
    $needle = "#include <sstream>"
    $replacement = @"
#include <sstream>

#ifndef itkOverrideGetNameOfClassMacro
#  define itkOverrideGetNameOfClassMacro(thisClass) \
    virtual const char * GetNameOfClass() const override { return #thisClass; }
#endif
"@
    if (-not $macroText.Contains($needle)) { throw "Cannot patch ITK macro header; include anchor not found." }
    $macroText = $macroText.Replace($needle, $replacement)
    Set-Content -Path $macroPath -Value $macroText -NoNewline
    Log "Patched ITK 5.3 compatibility macro for RTK."
}

Test-RequiredCommand cmake
Test-RequiredCommand tar

if ($ForceClean) {
    foreach ($dir in @((Join-Path $ItkRoot "build"), (Join-Path $RtkRoot "build"))) {
        if (Test-Path $dir) {
            Log "Removing $dir"
            Remove-Item -Recurse -Force $dir
        }
    }
}

$itkArchive = Join-Path $ItkRoot "ITK-$ItkVersion.tar.gz"
$rtkArchive = Join-Path $RtkRoot "RTK-$RtkVersion.tar.gz"
$itkSource = Join-Path $ItkRoot "src"
$rtkSource = Join-Path $RtkRoot "src"
$itkBuild = Join-Path $ItkRoot "build"
$rtkBuild = Join-Path $RtkRoot "build"

$ItkPlainVersion = $ItkVersion.TrimStart('v')
Save-AndExpandSourceArchive `
    -Url "https://github.com/InsightSoftwareConsortium/ITK/releases/download/$ItkVersion/InsightToolkit-$ItkPlainVersion.tar.gz" `
    -Archive $itkArchive `
    -Destination $ItkRoot `
    -FinalSourceDir $itkSource
Ensure-Itk-Rtk-Compatibility -ItkSourceDir $itkSource

Save-AndExpandSourceArchive `
    -Url "https://github.com/RTKConsortium/RTK/archive/refs/tags/$RtkVersion.tar.gz" `
    -Archive $rtkArchive `
    -Destination $RtkRoot `
    -FinalSourceDir $rtkSource

Log "Configuring ITK static build"
cmake -S $itkSource -B $itkBuild `
    -G "Visual Studio 15 2017" -A x64 `
    -DBUILD_SHARED_LIBS=OFF `
    -DBUILD_TESTING=OFF `
    -DBUILD_EXAMPLES=OFF `
    -DITK_BUILD_DEFAULT_MODULES=ON `
    -DModule_ITKReview=ON `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
if ($LASTEXITCODE -ne 0) { throw "ITK configure failed" }

Log "Building ITK static libraries"
cmake --build $itkBuild --config Release --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw "ITK build failed" }

Log "Configuring RTK static build against ITK build tree"
cmake -S $rtkSource -B $rtkBuild `
    -G "Visual Studio 15 2017" -A x64 `
    "-DITK_DIR=$itkBuild" `
    -DBUILD_SHARED_LIBS=OFF `
    -DRTK_BUILD_APPLICATIONS=OFF `
    -DBUILD_TESTING=OFF `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
if ($LASTEXITCODE -ne 0) { throw "RTK configure failed" }

Log "Building RTK static libraries"
cmake --build $rtkBuild --config Release --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw "RTK build failed" }

Log "RTK ready. Configure this project with:"
Log "  -DRTK_DIR=$rtkBuild"
