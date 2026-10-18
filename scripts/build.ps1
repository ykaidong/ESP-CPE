# ESP-CPE Phase 1 build helper (run in a normal PowerShell, NOT inside the AI sandbox)
#
# Usage:
#   .\scripts\build.ps1              # incremental build (sets target on first run)
#   .\scripts\build.ps1 -FullClean   # clean rebuild
#   .\scripts\build.ps1 -Flash COM5  # build + flash + monitor
#
# Requirements: ESP-IDF v6.1 installed with the Espressif Installation Manager
# (EIM). If your activation script lives elsewhere, edit $IdfProfile below.

param(
    [switch]$FullClean,
    [string]$FlashPort = ""
)

$ErrorActionPreference = "Stop"
$IdfProfile = "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

if (-not (Test-Path $IdfProfile)) {
    Write-Error "ESP-IDF activation script not found: $IdfProfile"
}
& $IdfProfile | Out-Null

if ($FullClean) {
    idf.py fullclean
}

if (-not (Test-Path "sdkconfig")) {
    idf.py set-target esp32s3
}

idf.py build 2>&1 | Tee-Object -FilePath "build_log.txt"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBUILD FAILED - please send build_log.txt (or its error section) to the assistant." -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "`nBUILD OK" -ForegroundColor Green
Get-ChildItem build\*.bin | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "Flash with: idf.py -p <COMx> flash monitor"

if ($FlashPort -ne "") {
    idf.py -p $FlashPort flash monitor
}
