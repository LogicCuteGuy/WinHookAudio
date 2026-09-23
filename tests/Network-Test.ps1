param([string]$NetworkTest)
if (-not $NetworkTest) { Write-Error "Missing -NetworkTest"; exit 1 }
if (-not (Test-Path $NetworkTest)) { Write-Error "Not found: $NetworkTest"; exit 1 }
$out = & $NetworkTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "network-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "network-test pass != true"; exit 1 }
Write-Output "network-test offline PASS"
exit 0
