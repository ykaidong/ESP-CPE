<#
.SYNOPSIS
    Phase 2 management-console security regression for the ESP-CPE firmware.

.DESCRIPTION
    Runs the HTTP checks from docs/PHASE2_TEST_PLAN.md section 1 against a
    device. The PC must be connected to the device SoftAP (default
    192.168.4.1). The lockout test is opt-in because it locks the admin account
    for 60 seconds.

.PARAMETER Device
    Device address, default 192.168.4.1.

.PARAMETER Password
    Current admin password. Required for the authenticated checks.

.PARAMETER IncludeLockout
    Also run the 5 wrong-password lockout test (takes ~60 s to clear).

.EXAMPLE
    .\scripts\web_security_test.ps1 -Password 'your-password'
#>
[CmdletBinding()]
param(
    [string]$Device = '192.168.4.1',
    [string]$Password,
    [switch]$IncludeLockout
)

$ErrorActionPreference = 'Stop'
$baseUri = "http://$Device"
$results = New-Object System.Collections.ArrayList

function Add-Result {
    param([string]$Name, [bool]$Pass, [string]$Detail)
    $null = $results.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
    $color = if ($Pass) { 'Green' } else { 'Red' }
    $tag = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1}: {2}" -f $tag, $Name, $Detail) -ForegroundColor $color
}

function Invoke-HttpChecked {
    param(
        [string]$Method,
        [string]$Uri,
        [hashtable]$Headers = @{},
        [string]$Body,
        [Microsoft.PowerShell.Commands.WebRequestSession]$Session
    )
    $params = @{
        UseBasicParsing = $true
        Method          = $Method
        Uri             = $Uri
        Headers         = $Headers
        TimeoutSec      = 5
    }
    if ($Body) { $params['Body'] = $Body }
    if ($Session) { $params['WebSession'] = $Session }
    try {
        $resp = Invoke-WebRequest @params
        return @{ Status = [int]$resp.StatusCode; Headers = $resp.Headers; Content = $resp.Content }
    } catch {
        $response = $_.Exception.Response
        if ($null -eq $response) { throw }
        $headers = @{}
        try {
            foreach ($key in $response.Headers.AllKeys) { $headers[$key] = $response.Headers[$key] }
        } catch { }
        return @{ Status = [int]$response.StatusCode; Headers = $headers; Content = '' }
    }
}

function Send-RawHttp {
    param([string]$Request)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.Connect($Device, 80)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Request)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $buffer = New-Object byte[] 4096
        $read = 0
        try { $read = $stream.Read($buffer, 0, $buffer.Length) } catch { $read = 0 }
        if ($read -le 0) { return '' }
        return [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
    } finally {
        $client.Close()
    }
}

Write-Host "Target: $baseUri" -ForegroundColor Cyan;

# 1. Unauthenticated status: 401 + Connection: close
$r = Invoke-HttpChecked -Method 'GET' -Uri "$baseUri/api/status"
$connClose = $false
foreach ($key in $r.Headers.Keys) {
    if ($key -eq 'Connection' -and ($r.Headers[$key] -match 'close')) { $connClose = $true }
}
$detail = "HTTP $($r.Status)"
if ($connClose) { $detail += ', Connection: close' }
Add-Result -Name 'unauth /api/status' -Pass (($r.Status -eq 401) -and $connClose) -Detail $detail

# 2. Oversized Content-Length must be rejected without stalling httpd
$raw = "POST /api/login HTTP/1.1`r`nHost: $Device`r`nContent-Type: application/json`r`nContent-Length: 2000000000`r`nConnection: close`r`n`r`n"
$rawResp = Send-RawHttp -Request $raw
$oversizedOk = $rawResp -match 'HTTP/1\.[01] (400|401|413)'
Add-Result -Name 'oversized Content-Length' -Pass $oversizedOk -Detail ($(if ($rawResp) { $rawResp.Split("`r`n")[0] } else { 'no response' }))

# 3. Malformed JSON (authenticated) must be 400
$loginOk = $false
$session = $null
$csrf = $null
if ($Password) {
    $session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    $loginBody = @{ username = 'admin'; password = $Password } | ConvertTo-Json -Compress
    $login = Invoke-HttpChecked -Method 'POST' -Uri "$baseUri/api/login" -Headers @{ 'Content-Type' = 'application/json' } -Body $loginBody -Session $session
    $loginOk = ($login.Status -eq 200)
    Add-Result -Name 'login' -Pass $loginOk -Detail "HTTP $($login.Status)"

    if ($loginOk) {
        $cookies = $session.Cookies.GetCookies([Uri]$baseUri)
        foreach ($cookie in $cookies) {
            if ($cookie.Name -eq 'CPE_CSRF') { $csrf = $cookie.Value }
        }
        if ($null -eq $csrf) {
            Add-Result -Name 'CSRF cookie' -Pass $false -Detail 'CPE_CSRF cookie missing after login'
        } else {
            Add-Result -Name 'CSRF cookie' -Pass $true -Detail 'present'
        }

        $bad = Invoke-HttpChecked -Method 'POST' -Uri "$baseUri/wlan_advance" -Headers @{ 'Content-Type' = 'application/json'; 'X-CSRF-Token' = "$csrf" } -Body '{"channel":' -Session $session
        Add-Result -Name 'malformed JSON' -Pass ($bad.Status -eq 400) -Detail "HTTP $($bad.Status)"

        # 4. Valid JSON but invalid channel must be 400 and must not brick config
        $invalid = Invoke-HttpChecked -Method 'POST' -Uri "$baseUri/wlan_advance" -Headers @{ 'Content-Type' = 'application/json'; 'X-CSRF-Token' = "$csrf" } -Body '{"channel":"999","bandwidth":"20"}' -Session $session
        Add-Result -Name 'invalid channel' -Pass ($invalid.Status -eq 400) -Detail "HTTP $($invalid.Status)"

        # 5. Missing CSRF must be 403
        $noCsrf = Invoke-HttpChecked -Method 'POST' -Uri "$baseUri/wlan_advance" -Headers @{ 'Content-Type' = 'application/json' } -Body '{"channel":"6","bandwidth":"20"}' -Session $session
        Add-Result -Name 'missing CSRF' -Pass ($noCsrf.Status -eq 403) -Detail "HTTP $($noCsrf.Status)"
    }
} else {
    Write-Host '[SKIP] authenticated checks (no -Password given)' -ForegroundColor Yellow
}

# 6. Optional lockout test (destructive: locks login for 60 s)
if ($IncludeLockout) {
    $locked = $false
    for ($i = 1; $i -le 6; $i++) {
        $body = @{ username = 'admin'; password = "__wrong_$i" } | ConvertTo-Json -Compress
        $resp = Invoke-HttpChecked -Method 'POST' -Uri "$baseUri/api/login" -Headers @{ 'Content-Type' = 'application/json' } -Body $body
        if ($resp.Status -eq 429) { $locked = $true; break }
    }
    Add-Result -Name 'login lockout (429)' -Pass $locked -Detail ($(if ($locked) { 'locked as expected' } else { 'no 429 after 6 attempts' }))
}

Write-Host ''
$failed = ($results | Where-Object { -not $_.Pass }).Count
if ($failed -gt 0) {
    Write-Host "RESULT: FAIL ($failed check(s))" -ForegroundColor Red
    exit 1
}
Write-Host 'RESULT: PASS' -ForegroundColor Green
exit 0
