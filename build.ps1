param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$BuildDirectory = '',

    [switch]$NoRun,

    [switch]$Clean = $false,

    [switch]$UseVcpkg = $false,

    [switch]$SkipOfflineBuild = $false,

    [switch]$SkipTests = $false
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$Message) {
    Write-Host "[build] $Message" -ForegroundColor Cyan
}

function Test-RequiredCommand([string]$Name, [string]$FallbackPath) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    if ($FallbackPath -and (Test-Path $FallbackPath)) { return $FallbackPath }
    throw "$Name was not found. Install it or add it to PATH."
}

function Get-VcVarsPath {
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
        if (Test-Path $candidate) { return $candidate }
    }

    throw 'No Visual Studio C++ developer environment was found.'
}

$sourceDir = (Resolve-Path $PSScriptRoot).Path
$externalDir = Join-Path $sourceDir 'external'
$offlineScript = Join-Path $externalDir 'BUILD_ALL_OFFLINE.ps1'

$vtkConfig = Join-Path $externalDir 'vtk\install\share\vtk\vtk-config.cmake'
$dcmtkConfig = Join-Path $externalDir 'dcmtk\install\cmake\DCMTKConfig.cmake'
$vtkReady = Test-Path $vtkConfig
$dcmtkReady = Test-Path $dcmtkConfig

Write-Step 'Checking local dependencies'
Write-Host "  VTK:   $(if ($vtkReady) { 'ready' } else { 'missing' })"
Write-Host "  DCMTK: $(if ($dcmtkReady) { 'ready' } else { 'missing' })"

if (-not ($vtkReady -and $dcmtkReady)) {
    if ($SkipOfflineBuild) {
        throw 'Required local dependencies are missing and -SkipOfflineBuild was specified.'
    }
    if (-not (Test-Path $offlineScript)) {
        throw "Offline dependency script not found: $offlineScript"
    }

    $skipArgs = @()
    if ($vtkReady) { $skipArgs += '-SkipVTK' }
    if ($dcmtkReady) { $skipArgs += '-SkipDCMTK' }

    Write-Step "Running offline dependency build: $offlineScript $($skipArgs -join ' ')"
    & $offlineScript @skipArgs
    if ($LASTEXITCODE -ne 0) { throw "Offline dependency build failed with exit code $LASTEXITCODE." }
} else {
    Write-Step 'Local VTK and DCMTK are ready; skipping offline dependency build'
}

$cmake = Test-RequiredCommand 'cmake' 'C:\Program Files\CMake\bin\cmake.exe'
$ctest = Test-RequiredCommand 'ctest' 'C:\Program Files\CMake\bin\ctest.exe'
$vcvars = Get-VcVarsPath

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = 'build-vs2017-x64'
}

$buildDir = Join-Path $sourceDir $BuildDirectory
$executable = Join-Path $buildDir "$Configuration\hello_cross_platform.exe"
$vtkBin = Join-Path $sourceDir 'external\vtk\install\bin'
$dcmtkBin = Join-Path $sourceDir 'external\dcmtk\install\bin'
$vcpkgBin = 'C:\tools\vcpkg\installed\x64-windows\bin'
$vcpkgToolchain = 'C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake'
$rtkBuild = Join-Path $sourceDir 'external\rtk\build'

$configureArgs = @(
    '-S', $sourceDir,
    '-B', $buildDir,
    '-G', 'Visual Studio 15 2017',
    '-A', 'x64',
    '-DUSE_LOCAL_DEPENDENCIES=ON'
)

if ($SkipTests) {
    $configureArgs += '-DBUILD_TESTING=OFF'
} else {
    $configureArgs += '-DBUILD_TESTING=ON'
}

if (Test-Path $vcpkgToolchain) {
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
}

if (Test-Path $rtkBuild) {
    $configureArgs += "-DRTK_DIR=$rtkBuild"
}

$quotedConfigureArgs = ($configureArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '

$buildTarget = if ($SkipTests) { 'hello_cross_platform' } else { 'ALL_BUILD' }
$buildCommand = "`"$cmake`" --build `"$buildDir`" --config $Configuration --target $buildTarget"
if ($Clean) {
    $buildCommand += ' --clean-first'
}
$buildCommand += ' --parallel 4'

$cmdParts = @(
    "call `"$vcvars`" >nul 2>&1",
    "set `"PATH=$vtkBin;$dcmtkBin;$vcpkgBin;%PATH%`"",
    "`"$cmake`" $quotedConfigureArgs",
    $buildCommand
)

if (-not $SkipTests) {
    $cmdParts += "`"$ctest`" --test-dir `"$buildDir`" -C $Configuration --output-on-failure"
}

if (-not $NoRun) {
    $cmdParts += "`"$executable`" --gui"
}

$cmd = $cmdParts -join ' && '

Write-Step "CMake: $cmake"
Write-Step "vcvars: $vcvars"
Write-Step "Build directory: $buildDir"
Write-Step "Configuration: $Configuration"
Write-Step "Clean rebuild: $(if ($Clean) { 'enabled' } else { 'disabled' })"
Write-Step "Build target: $buildTarget"
Write-Step "Runtime PATH prefix: $vtkBin; $dcmtkBin; $vcpkgBin"
Write-Step "Configure command uses VS2017 x64, local dependencies, vcpkg toolchain when present, and RTK build tree when present"

cmd /c $cmd

if ($LASTEXITCODE -ne 0) {
    if ($LASTEXITCODE -eq -1073741515) {
        throw "Program launch failed with exit code -1073741515 (0xC0000135). This usually means a runtime DLL is missing. Try -NoRun to skip launching the GUI."
    }
    throw "Build script failed with exit code $LASTEXITCODE. Build directory: $buildDir"
}

Write-Step 'Build script completed successfully'
