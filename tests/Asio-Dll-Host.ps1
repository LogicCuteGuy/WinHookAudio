param([string]$DllHost, [string]$Master, [string]$Bridge)
if (-not $DllHost -or -not $Master -or -not $Bridge) { Write-Error "Missing -DllHost/-Master/-Bridge"; exit 1 }
foreach ($p in $DllHost, $Master, $Bridge) { if (-not (Test-Path $p)) { Write-Error "Not found: $p"; exit 1 } }
$out = & $DllHost $Master $Bridge 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "asio-dll-host failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "asio-dll-host pass != true"; exit 1 }
Write-Output "asio-dll-host offline PASS"
exit 0
