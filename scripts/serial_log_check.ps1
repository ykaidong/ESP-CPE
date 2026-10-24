<#
.SYNOPSIS
    Phase 2 serial log checker for the ESP-CPE firmware.

.DESCRIPTION
    Counts the interesting boot/recovery events and fails when the log contains
    a panic/backtrace or the periodic "existing device(s)" log spam that was
    fixed in iot_usbh_cdc 3.1.2.

.PARAMETER LogFile
    Path to a file containing the serial monitor output.

.EXAMPLE
    .\scripts\serial_log_check.ps1 -LogFile .\build_log.txt
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LogFile
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $LogFile)) {
    Write-Error "Log file not found: $LogFile"
    exit 2
}

$text = Get-Content -LiteralPath $LogFile -Raw
if ($null -eq $text) {
    Write-Error "Log file is empty: $LogFile"
    exit 2
}

function Count-Pattern {
    param([string]$Pattern)
    $matches = [regex]::Matches($text, $Pattern)
    return $matches.Count
}

$stats = [ordered]@{
    'New device connected'                 = (Count-Pattern 'New device connected, address:')
    'ECM interface found'                  = (Count-Pattern 'ECM interface found:')
    '4G WAN IP'                            = (Count-Pattern '4G WAN IP:')
    'CPE online'                           = (Count-Pattern 'CPE online \(WAN IP')
    'Link up but no IP'                    = (Count-Pattern 'Link up but no IP after')
    '  -> ECM rebuild'                     = (Count-Pattern 'rebuilding the ECM driver')
    '  -> 4G module restart'               = (Count-Pattern 'restarting the 4G module')
    '  -> module reset in hardware'        = (Count-Pattern 'resetting it in hardware')
    'IP but no data connection'            = (Count-Pattern 'but no data connection:')
    '  -> PDP escalation'                  = (Count-Pattern 'No data for \d+ ms with IP')
    'No USB link'                          = (Count-Pattern 'No USB link after')
    'USB root port power cycles'           = (Count-Pattern 'Power-cycling the USB root port')
    'ECM link stuck'                       = (Count-Pattern 'ECM link stuck')
    'Boot counter cleared'                 = (Count-Pattern 'Healthy for \d+ s, boot counter cleared')
    'ECM teardown incomplete'              = (Count-Pattern 'ECM teardown incomplete')
    'Scheduled ECM rebuild'                = (Count-Pattern 'Scheduled ECM rebuild')
    'Scheduled reboot'                     = (Count-Pattern 'Scheduled reboot #')
    'Replay (device actually delivered)'   = (Count-Pattern 'Replaying the CONNECTED event for device')
    'CHECK_SHORT_DEV_DESC'                 = (Count-Pattern 'CHECK_SHORT_DEV_DESC')
}

$panic = Count-Pattern 'Guru Meditation|Backtrace:|assert failed|abort\(\)|TASK_WDT|Interrupt wdt'
$spam = Count-Pattern 'Replaying the CONNECTED event for \d+ existing device\(s\)'
$resetCount = Count-Pattern 'rst:0x'

Write-Host ''
Write-Host '=== ESP-CPE Phase 2 log check ===' -ForegroundColor Cyan
Write-Host ("File: {0} ({1:N0} bytes)" -f (Resolve-Path -LiteralPath $LogFile), $text.Length)
Write-Host ''
foreach ($key in $stats.Keys) {
    Write-Host ("  {0,-38} {1,6}" -f $key, $stats[$key])
}
Write-Host ("  {0,-38} {1,6}" -f 'Boot/reset markers', $resetCount)
Write-Host ''

$failed = $false
if ($panic -gt 0) {
    Write-Host "FAIL: panic/assert/watchdog pattern found ($panic occurrences)" -ForegroundColor Red
    $failed = $true
}
if ($spam -gt 0) {
    Write-Host "FAIL: periodic 'existing device(s)' log spam found ($spam occurrences); expected 0 on iot_usbh_cdc 3.1.2" -ForegroundColor Red
    $failed = $true
}
if ($stats['Replay (device actually delivered)'] -gt 5) {
    Write-Host "WARN: more than 5 replayed devices; check for repeated event loss or re-enumeration" -ForegroundColor Yellow
}
if ($stats['ECM teardown incomplete'] -gt 0) {
    Write-Host "WARN: an ECM teardown did not complete; a rebuild counted as successful may not have happened" -ForegroundColor Yellow
}
# The modem restart budget is per "no IP" episode (CONFIG_CPE_IP_MODEM_RESETS,
# at most 5), so a log with many of them means the link never stayed up.
if ($stats['  -> 4G module restart'] -ge 10) {
    Write-Host "WARN: the 4G module was restarted $($stats['  -> 4G module restart']) times; check that the link recovers at all" -ForegroundColor Yellow
}

if ($failed) {
    Write-Host 'RESULT: FAIL' -ForegroundColor Red
    exit 1
}

Write-Host 'RESULT: PASS' -ForegroundColor Green
exit 0
