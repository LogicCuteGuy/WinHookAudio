param([string]$WorkerTest)
if (-not $WorkerTest) { Write-Error "Missing -WorkerTest"; exit 1 }
if (-not (Test-Path $WorkerTest)) { Write-Error "Not found: $WorkerTest"; exit 1 }
$out = & $WorkerTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "worker-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "worker-test pass != true"; exit 1 }
Write-Output "worker-test offline PASS"
exit 0
