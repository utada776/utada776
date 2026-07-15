$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $ScriptDir "src"
$full = Join-Path $ScriptDir "RTK-2.7.0"
if ((Test-Path $full) -and (Test-Path (Join-Path $full "include"))) {
    if (Test-Path $src) { Remove-Item -Recurse -Force $src }
    Move-Item $full $src
}
$archive = Join-Path $ScriptDir "RTK-v2.7.0.tar.gz"
Remove-Item $archive -ErrorAction SilentlyContinue
if (!(Test-Path (Join-Path $src "include"))) {
    throw "external/rtk/src is not a complete RTK source tree."
}
Write-Host "RTK source normalized: $src"
