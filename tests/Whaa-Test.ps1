param([string]$WhaaTest)
if (-not $WhaaTest) { Write-Error "Missing -WhaaTest"; exit 1 }
if (-not (Test-Path $WhaaTest)) { Write-Error "Not found: $WhaaTest"; exit 1 }
$out = & $WhaaTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "whaa-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "whaa-test pass != true"; exit 1 }
Write-Output "whaa-test offline PASS"
exit 0
