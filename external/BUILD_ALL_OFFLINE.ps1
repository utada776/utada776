# BUILD_ALL_OFFLINE.ps1
# 自动下载、编译和安装 VTK 和 DCMTK
# 使用: cd external; .\BUILD_ALL_OFFLINE.ps1

param(
    [switch]$SkipDownload = $false,      # 跳过下载（已有源代码）
    [switch]$SkipVTK = $false,            # 跳过 VTK 编译
    [switch]$SkipDCMTK = $false,          # 跳过 DCMTK 编译
    [int]$Parallel = 4,                   # 并行编译数
    [switch]$ForceClean = $false          # 强制清理 build/install 后全量重编
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$StartTime = Get-Date

function Log {
    param([string]$Message, [string]$Level = "INFO")
    $Timestamp = Get-Date -Format "HH:mm:ss"
    Write-Host "[$Timestamp] [$Level] $Message"
}

function LogSection {
    param([string]$Title)
    Write-Host ""
    Write-Host "╔═══════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
    Write-Host "║ $($Title.PadRight(57)) ║" -ForegroundColor Cyan
    Write-Host "╚═══════════════════════════════════════════════════════════╝" -ForegroundColor Cyan
    Write-Host ""
}

function Invoke-CMakeBuildWithProgress {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [Parameter(Mandatory = $true)]
        [int]$Parallel,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    Log "$Label：启用详细构建输出（MSBuild /v:m + PerformanceSummary）"
    Log "如果终端短时间没有新行，通常仍在编译大型源文件或链接阶段，请以 CPU 持续增长为准。"

    & cmake --build $BuildDirectory --config $Configuration --parallel $Parallel -- /v:m "/clp:PerformanceSummary;Summary"
    return $LASTEXITCODE
}

function TestCMake {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        Log "CMake 未找到！请先安装 CMake 3.16+" "ERROR"
        exit 1
    }
    Log "CMake 已找到: $($cmake.Source)"
}

function TestVisualStudio {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\Community',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional'
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) {
        Log "Visual Studio (2017/2019/2022) 未找到！请安装 Visual Studio C++ 工具集" "ERROR"
        throw "Visual Studio not found"
    }
    Log "Visual Studio 已找到: $found"
}

function DownloadVTK {
    LogSection "下载 VTK 9.3.0"

    if (Test-Path "$ScriptDir\vtk\src") {
        Log "VTK 源代码已存在，跳过下载"
        return $true
    }

    if ($SkipDownload) {
        Log "已指定 -SkipDownload，跳过 VTK 下载" "WARN"
        return $false
    }
    
    $url = "https://github.com/Kitware/VTK/archive/refs/tags/v9.3.0.tar.gz"
    $outFile = "$ScriptDir\vtk\VTK-9.3.0.tar.gz"
    
    Log "下载地址: $url"
    Log "保存位置: $outFile"
    Log "这可能需要几分钟..."
    
    try {
        Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing -TimeoutSec 300
        Log "✓ VTK 下载完成"
    } catch {
        Log "✗ VTK 下载失败: $_" "ERROR"
        Log "备选方案：手动从 https://github.com/Kitware/VTK/releases 下载" "WARN"
        return $false
    }
    
    Log "解压 VTK..."
    tar -xzf $outFile -C "$ScriptDir\vtk"
    if ($LASTEXITCODE -ne 0) {
        Log "✗ VTK 解压失败" "ERROR"
        return $false
    }
    
    # 等待文件释放，防止文件被占用
    Start-Sleep -Seconds 2
    $srcPath = "$ScriptDir\vtk\VTK-9.3.0"
    $destPath = "$ScriptDir\vtk\src"
    if (Test-Path $destPath) {
        Remove-Item $destPath -Recurse -Force
    }
    
    try {
        Move-Item $srcPath $destPath -Force -ErrorAction Stop
    } catch {
        Log "⚠ Move-Item 失败，尝试重命名..." "WARN"
        if (Test-Path $srcPath) {
            Rename-Item $srcPath "src" -Force
        }
    }
    
    Remove-Item $outFile -ErrorAction SilentlyContinue
    
    Log "✓ VTK 解压完成"
    return $true
}

function DownloadDCMTK {
    LogSection "下载 DCMTK 3.7.0"

    if (Test-Path "$ScriptDir\dcmtk\src") {
        Log "DCMTK 源代码已存在，跳过下载"
        return $true
    }

    if ($SkipDownload) {
        Log "已指定 -SkipDownload，跳过 DCMTK 下载" "WARN"
        return $false
    }
    
    $url = "https://github.com/DCMTK/dcmtk/archive/refs/tags/DCMTK-3.7.0.tar.gz"
    $outFile = "$ScriptDir\dcmtk\DCMTK-3.7.0.tar.gz"
    
    Log "下载地址: $url"
    Log "保存位置: $outFile"
    Log "这可能需要几分钟..."
    
    try {
        Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing -TimeoutSec 300
        Log "✓ DCMTK 下载完成"
    } catch {
        Log "✗ DCMTK 下载失败: $_" "ERROR"
        Log "备选方案：手动从 https://github.com/DCMTK/dcmtk/releases 下载" "WARN"
        return $false
    }
    
    Log "解压 DCMTK..."
    tar -xzf $outFile -C "$ScriptDir\dcmtk"
    if ($LASTEXITCODE -ne 0) {
        Log "✗ DCMTK 解压失败" "ERROR"
        return $false
    }
    
    # 等待文件释放，防止文件被占用
    Start-Sleep -Seconds 2
    $srcPath = "$ScriptDir\dcmtk\dcmtk-DCMTK-3.7.0"
    $destPath = "$ScriptDir\dcmtk\src"
    if (Test-Path $destPath) {
        Remove-Item $destPath -Recurse -Force
    }
    
    try {
        Move-Item $srcPath $destPath -Force -ErrorAction Stop
    } catch {
        Log "⚠ Move-Item 失败，尝试重命名..." "WARN"
        if (Test-Path $srcPath) {
            Rename-Item $srcPath "src" -Force
        }
    }
    
    Remove-Item $outFile -ErrorAction SilentlyContinue
    
    Log "✓ DCMTK 解压完成"
    return $true
}

function BuildVTK {
    if ($SkipVTK) {
        Log "跳过 VTK 编译"
        return $true
    }
    
    LogSection "编译 VTK 9.3.0 (约 30-60 分钟)"
    
    $buildDir = "$ScriptDir\vtk\build"
    $installDir = "$ScriptDir\vtk\install"
    $vtkConfig = "$installDir\lib\cmake\VTK\VTKConfig.cmake"

    if (Test-Path $vtkConfig) {
        Log "检测到已安装 VTK（$vtkConfig），跳过重新编译"
        return $true
    }
    
    if (-not (Test-Path "$ScriptDir\vtk\src")) {
        Log "✗ VTK 源代码不存在" "ERROR"
        return $false
    }
    
    if ($ForceClean) {
        Log "已指定 -ForceClean，清理旧的编译与安装目录..."
        if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
        if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
    } else {
        Log "增量模式：保留现有 build/install 目录"
    }

    mkdir -Force $buildDir | Out-Null
    mkdir -Force $installDir | Out-Null
    
    Log "配置 CMake..."
    Push-Location $buildDir
    
    $cmakeCmd = @(
        "..\src",
        "-G", "Visual Studio 15 2017",
        "-A", "x64",
        "-DCMAKE_INSTALL_PREFIX=$installDir",
        "-DBUILD_SHARED_LIBS=ON",
        "-DVTK_GROUP_RENDERING=ON",
        "-DVTK_GROUP_IMAGING=ON",
        "-DVTK_WRAP_PYTHON=OFF",
        "-DBUILD_TESTING=OFF",
        "-DCMAKE_BUILD_TYPE=Release"
    )
    
    & cmake @cmakeCmd
    if ($LASTEXITCODE -ne 0) {
        Log "✗ CMake 配置失败" "ERROR"
        Pop-Location
        return $false
    }
    
    Log "编译 VTK（并行数: $Parallel）..."
    $buildStart = Get-Date
    Invoke-CMakeBuildWithProgress -BuildDirectory "." -Configuration "Release" -Parallel $Parallel -Label "VTK 编译"
    if ($LASTEXITCODE -ne 0) {
        Log "✗ VTK 编译失败" "ERROR"
        Pop-Location
        return $false
    }
    $buildDuration = (Get-Date) - $buildStart
    Log "✓ VTK 编译完成 (耗时: $($buildDuration.ToString('mm\:ss')))"
    
    Log "安装 VTK..."
    & cmake --install . --config Release
    if ($LASTEXITCODE -ne 0) {
        Log "✗ VTK 安装失败" "ERROR"
        Pop-Location
        return $false
    }
    
    Pop-Location
    
    # 验证安装
    $configFile = "$installDir\lib\cmake\VTK\VTKConfig.cmake"
    if (Test-Path $configFile) {
        Log "✓ VTK 安装验证成功"
        return $true
    } else {
        Log "✗ VTK 安装验证失败" "ERROR"
        return $false
    }
}

function BuildDCMTK {
    if ($SkipDCMTK) {
        Log "跳过 DCMTK 编译"
        return $true
    }
    
    LogSection "编译 DCMTK 3.7.0 (约 15-30 分钟)"
    
    $buildDir = "$ScriptDir\dcmtk\build"
    $installDir = "$ScriptDir\dcmtk\install"
    $dcmtkConfig = "$installDir\lib\cmake\dcmtk\DCMTKConfig.cmake"

    if (Test-Path $dcmtkConfig) {
        Log "检测到已安装 DCMTK（$dcmtkConfig），跳过重新编译"
        return $true
    }
    
    if (-not (Test-Path "$ScriptDir\dcmtk\src")) {
        Log "✗ DCMTK 源代码不存在" "ERROR"
        return $false
    }
    
    if ($ForceClean) {
        Log "已指定 -ForceClean，清理旧的编译与安装目录..."
        if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
        if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
    } else {
        Log "增量模式：保留现有 build/install 目录"
    }

    mkdir -Force $buildDir | Out-Null
    mkdir -Force $installDir | Out-Null
    
    Log "配置 CMake..."
    Push-Location $buildDir
    
    $cmakeCmd = @(
        "..\src",
        "-G", "Visual Studio 15 2017",
        "-A", "x64",
        "-DCMAKE_INSTALL_PREFIX=$installDir",
        "-DBUILD_SHARED_LIBS=ON",
        "-DDCMTK_WITH_ZLIB=ON",
        "-DBUILD_TESTING=OFF",
        "-DDCMTK_ENABLE_PRIVATE_TAGS=ON",
        "-DCMAKE_BUILD_TYPE=Release"
    )
    
    & cmake @cmakeCmd
    if ($LASTEXITCODE -ne 0) {
        Log "✗ CMake 配置失败" "ERROR"
        Pop-Location
        return $false
    }
    
    Log "编译 DCMTK（并行数: $Parallel）..."
    $buildStart = Get-Date
    Invoke-CMakeBuildWithProgress -BuildDirectory "." -Configuration "Release" -Parallel $Parallel -Label "DCMTK 编译"
    if ($LASTEXITCODE -ne 0) {
        Log "✗ DCMTK 编译失败" "ERROR"
        Pop-Location
        return $false
    }
    $buildDuration = (Get-Date) - $buildStart
    Log "✓ DCMTK 编译完成 (耗时: $($buildDuration.ToString('mm\:ss')))"
    
    Log "安装 DCMTK..."
    & cmake --install . --config Release
    if ($LASTEXITCODE -ne 0) {
        Log "✗ DCMTK 安装失败" "ERROR"
        Pop-Location
        return $false
    }
    
    Pop-Location
    
    # 验证安装
    $configFile = "$installDir\lib\cmake\dcmtk\DCMTKConfig.cmake"
    $dataLib = "$installDir\lib\dcmdata.lib"
    if ((Test-Path $configFile) -and (Test-Path $dataLib)) {
        Log "✓ DCMTK 安装验证成功"
        return $true
    } else {
        Log "✗ DCMTK 安装验证失败" "ERROR"
        return $false
    }
}

function PrintSummary {
    param([PSCustomObject]$Results)
    
    LogSection "编译总结"
    
    $totalTime = (Get-Date) - $StartTime
    
        if ($Results.VTK)   { Log "VTK 编译:   ✓ 成功" } else { Log "VTK 编译:   ✗ 失败" "ERROR" }
        if ($Results.DCMTK) { Log "DCMTK 编译: ✓ 成功" } else { Log "DCMTK 编译: ✗ 失败" "ERROR" }
    Log "总耗时: $($totalTime.ToString('hh\:mm\:ss'))"
    
    if ($Results.VTK -and $Results.DCMTK) {
        Write-Host ""
        Log "🎉 所有依赖编译完成！" "SUCCESS"
        Log "下一步："
        Log "  1. cd .. (返回项目根目录)"
        Log "  2. cmake -S . -B build-vs2017-x64 -A x64 -DUSE_LOCAL_DEPENDENCIES=ON"
        Log "  3. cmake --build build-vs2017-x64 --config Release"
    } else {
        Log "❌ 部分编译失败，请检查上面的错误信息" "ERROR"
    }
}

function Main {
    Write-Host ""
    Log "╔═ FDK 项目离线依赖编译脚本 ═╗"
    
    TestCMake
    TestVisualStudio
    
    $results = [PSCustomObject]@{
        VTK = $true
        DCMTK = $true
    }
    
    # 下载阶段（按 SkipVTK/SkipDCMTK 精确控制）
    if (-not $SkipDownload) {
        if (-not $SkipVTK) {
            DownloadVTK | Out-Null
        } else {
            Log "跳过 VTK 下载（-SkipVTK）"
        }

        if (-not $SkipDCMTK) {
            DownloadDCMTK | Out-Null
        } else {
            Log "跳过 DCMTK 下载（-SkipDCMTK）"
        }
    }
    
    # 编译阶段
    $results.VTK = BuildVTK
    $results.DCMTK = BuildDCMTK
    
    # 总结
    PrintSummary $results
}

Main
