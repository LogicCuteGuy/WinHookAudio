param([string]$KsTest)
if (-not $KsTest) { Write-Error "Missing -KsTest"; exit 1 }
if (-not (Test-Path $KsTest)) { Write-Error "Not found: $KsTest"; exit 1 }
$out = & $KsTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "ks-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "ks-test pass != true"; exit 1 }
Write-Output "ks-test offline PASS"
exit 0
