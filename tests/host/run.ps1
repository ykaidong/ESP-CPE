# Compiles and runs the host unit tests with gcc (MinGW).
#
# These tests cover the pure logic of the firmware (JSON handling, MAC parsing,
# configuration validation) against stub headers instead of ESP-IDF, so they run
# in a normal process: no board, no flashing, seconds instead of minutes.
# Nothing here is part of the firmware build - no component lists this directory.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tests/host/run.ps1

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is absolute; $MyInvocation can be relative when started with -File.
$here  = $PSScriptRoot
# tests/host -> tests -> repository root
$root  = Split-Path -Parent (Split-Path -Parent $here)
$out   = Join-Path $here 'build'
$bin   = Join-Path $out 'host_tests.exe'

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host 'ERROR: gcc not found in PATH (MinGW is expected on Windows).' -ForegroundColor Red
    exit 1
}

$cjson = Join-Path $root 'managed_components/espressif__cjson/cJSON'
if (-not (Test-Path (Join-Path $cjson 'cJSON.c'))) {
    Write-Host "ERROR: cJSON sources not found at $cjson; run idf.py build once so they are fetched." -ForegroundColor Red
    Write-Host "       (script dir '$here', repo root '$root')"
    exit 1
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

$includes = @(
    "-I$here/stubs",
    "-I$here",
    "-I$root/components/cpe_core/include",
    "-I$root/components/cpe_core/private_include",
    "-I$root/components/cpe_sms/include",
    "-I$root/components/web_console/include",
    "-I$root/components/web_console/router_web/private_include",
    "-I$cjson"
)

# Sources under test plus the real helpers they depend on; everything else that
# is referenced comes from stubs/stubs.c.
$sources = @(
    "$here/main.c",
    "$here/test_harness.c",
    "$here/stubs/stubs.c",
    "$here/test_json_util.c",
    "$here/test_station_names.c",
    "$here/test_config_validate.c",
    "$here/test_sms.c",
    "$root/components/cpe_sms/cpe_sms_parse.c",
    "$root/components/cpe_sms/cpe_sms_ucs2.c",
    "$root/components/web_console/router_web/json_util.c",
    "$root/components/web_console/router_web/station_names.c",
    "$root/components/cpe_core/cpe_config.c",
    "$root/components/cpe_core/cpe_ct.c",
    "$cjson/cJSON.c"
)

Write-Host 'compiling...'
& gcc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter $includes $sources -o $bin
if ($LASTEXITCODE -ne 0) {
    Write-Host "compilation failed (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host 'running...'
& $bin
$code = $LASTEXITCODE
if ($code -eq 0) {
    Write-Host 'host tests passed' -ForegroundColor Green
} else {
    Write-Host "host tests failed (exit $code)" -ForegroundColor Red
}
exit $code
