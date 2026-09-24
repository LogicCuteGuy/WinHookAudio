# Stress-Cable.ps1 - hammers WinHookAudio.sys (the Virtual Cable driver) on a TEST PC, meant to run
# with Driver Verifier on (verifier /standard /driver WinHookAudio.sys, then restart). A driver bug
# then stops Windows with a blue screen and C:\Windows\MEMORY.DMP instead of hiding. Not a ctest.
#
#   tests\Stress-Cable.ps1 [-Rounds 3] [-Block 1024] [-PnP] [-Reinstall]
#
# Every round:
#   1. all 8 cables stream both ways (winhookaudio-cable-live, 3 s each)
#   2. formats on cable 1: 44.1 / 48 / 96 kHz x 2 / 8 channels x float, 16, 24, 32, 24-in-32 bit
#   3. the client is killed mid-stream, then a new client must work (driver clean-up path)
# -Block: the tester's Worker block. Its Worker is a plain thread paced by Sleep(1), not MMCSS, so at
#   256 frames it sometimes loses audio on this VM (2 in 10 runs; 0 in 10 at 1024). The default 1024
#   keeps that timing issue out of a crash hunt; use 256 to measure the margin itself.
# -PnP (admin): disable / enable and restart the device while a stream runs, then stream again.
# -Reinstall (admin, once at the end): winhookaudio-devsetup remove + install from build\driver.
#
# Needs: the driver installed, and nothing else using the cable control device (close the DAWs:
# the Master's Worker takes the only client place). Changes the cable endpoints' Windows format;
# the next Master start sets it back to the Master Clock. Log: build\stress-cable-<time>.log
param(
    [int]$Rounds = 3,
    [int]$Block = 1024,
    [switch]$PnP,
    [switch]$Reinstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cableLive = Join-Path $root 'build\Release\winhookaudio-cable-live.exe'
$devsetup = Join-Path $root 'build\Release\winhookaudio-devsetup.exe'
$log = Join-Path $root ("build\stress-cable-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
foreach ($f in $cableLive, $devsetup) { if (-not (Test-Path $f)) { throw "$f not found: build Release first." } }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (($PnP -or $Reinstall) -and -not $admin) { throw '-PnP and -Reinstall need an Administrator PowerShell.' }

$script:passed = 0
$script:failed = @()
function Log([string]$text) { Write-Host $text; Add-Content -Path $log -Value $text }

# One cable-live run; records pass / fail with its last lines.
function Stream([string]$name, [string[]]$arguments) {
    $out = & $cableLive @arguments '--block' $Block 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) { $script:passed++; Log "  ok   $name" }
    else {
        $script:failed += $name
        Log "  FAIL $name (exit $LASTEXITCODE)"
        ($out -split "`r?`n" | Where-Object { $_ -match 'FAIL|HRESULT|error' } | Select-Object -First 6) |
            ForEach-Object { Log "       $_" }
    }
}

# Starts cable-live in the background (for kill / PnP tests).
function StartStream([double]$seconds) {
    Start-Process -FilePath $cableLive -ArgumentList '--seconds', $seconds, '--cable', '1', '--block', $Block -WindowStyle Hidden -PassThru
}

function DeviceId {
    (Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\WinHookAudio' } | Select-Object -First 1).InstanceId
}

Log "WinHookAudio.sys stress $(Get-Date -Format s), rounds $Rounds, block $Block, PnP $PnP, reinstall $Reinstall"
$verifier = (& verifier.exe /querysettings 2>&1 | Out-String)
Log ("Driver Verifier on WinHookAudio.sys: " + $(if ($verifier -match 'WinHookAudio\.sys') { 'YES' } else { 'no (bugs may stay hidden)' }))
Log (& $devsetup status | Out-String).TrimEnd()
if (Get-Process -Name 'Bitwig*', 'FL64', 'FL' -ErrorAction SilentlyContinue) {
    Log 'A DAW is running: close it first (it holds the cable control device).'
    exit 1
}

for ($round = 1; $round -le $Rounds; $round++) {
    Log "Round $round / $Rounds"
    for ($cable = 1; $cable -le 8; $cable++) {
        Stream "cable $cable 48k 2ch float" @('--cable', $cable, '--seconds', '3', '--rate', '48000')
    }
    foreach ($rate in 44100, 48000, 96000) {
        foreach ($channels in 2, 8) {
            foreach ($format in 0..4) {
                Stream "cable 1 $rate Hz ${channels}ch format $format" @('--cable', '1', '--seconds', '3',
                    '--rate', $rate, '--channels', $channels, '--format', $format)
            }
        }
    }
    $p = StartStream 10
    Start-Sleep -Milliseconds 2500
    Stop-Process -Id $p.Id -Force
    Log '  killed a client mid-stream'
    Start-Sleep -Milliseconds 500
    Stream 'new client after the kill' @('--cable', '1', '--seconds', '3')

    if ($PnP) {
        $id = DeviceId
        $p = StartStream 10
        Start-Sleep -Milliseconds 2500
        & pnputil.exe /disable-device $id | Out-Null
        Log "  disabled the device while streaming (pnputil exit $LASTEXITCODE)"
        Start-Sleep -Seconds 2
        & pnputil.exe /enable-device $id | Out-Null
        Log "  enabled the device (pnputil exit $LASTEXITCODE)"
        Wait-Process -Id $p.Id -Timeout 20 -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
        Stream 'stream after disable / enable' @('--cable', '1', '--seconds', '3')
        $p = StartStream 10
        Start-Sleep -Milliseconds 2500
        & pnputil.exe /restart-device $id | Out-Null
        Log "  restarted the device while streaming (pnputil exit $LASTEXITCODE)"
        Wait-Process -Id $p.Id -Timeout 20 -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
        Stream 'stream after restart' @('--cable', '1', '--seconds', '3')
    }
}

if ($Reinstall) {
    Log 'Reinstall'
    & $devsetup remove | Out-String | ForEach-Object { Log $_.TrimEnd() }
    Start-Sleep -Seconds 2
    & $devsetup install (Join-Path $root 'build\driver\WinHookAudio.inf') | Out-String | ForEach-Object { Log $_.TrimEnd() }
    Log "  devsetup install exit $LASTEXITCODE"
    Start-Sleep -Seconds 5
    Stream 'stream after reinstall' @('--cable', '1', '--seconds', '3')
}

Log (& $devsetup status | Out-String).TrimEnd()
Log "Done: $($script:passed) passed, $($script:failed.Count) failed. Log: $log"
if ($script:failed.Count) { exit 1 }
