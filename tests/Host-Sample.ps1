param([string]$HostSample)
if (-not $HostSample) { Write-Error "Missing -HostSample"; exit 1 }
if (-not (Test-Path $HostSample)) { Write-Error "Not found: $HostSample"; exit 1 }
$out = & $HostSample 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "host-sample failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "host-sample pass != true"; exit 1 }
if ($out -notmatch '"stream_verified":false') { Write-Error "stream_verified != false"; exit 1 }
Write-Output "host-sample offline PASS"
exit 0
