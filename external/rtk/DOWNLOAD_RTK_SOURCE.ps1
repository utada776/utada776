$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path $ScriptDir | Out-Null
$src = Join-Path $ScriptDir "src"
if (Test-Path $src) {
    Write-Host "RTK source already exists: $src"
    exit 0
}
$archive = Join-Path $ScriptDir "RTK-v2.7.0.tar.gz"
Write-Host "Downloading RTK v2.7.0..."
Invoke-WebRequest -Uri "https://github.com/RTKConsortium/RTK/archive/refs/tags/v2.7.0.tar.gz" -OutFile $archive -UseBasicParsing -TimeoutSec 900
Write-Host "Extracting RTK..."
tar -xzf $archive -C $ScriptDir
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
$dir = Get-ChildItem $ScriptDir -Directory | Where-Object { $_.Name -like "RTK-*" } | Select-Object -First 1
if (-not $dir) { throw "Extracted RTK directory was not found." }
Move-Item $dir.FullName $src
Remove-Item $archive -ErrorAction SilentlyContinue
Write-Host "RTK source ready: $src"
