param([string]$VirtualTest)
if (-not $VirtualTest) { Write-Error "Missing -VirtualTest"; exit 1 }
if (-not (Test-Path $VirtualTest)) { Write-Error "Not found: $VirtualTest"; exit 1 }
$out = & $VirtualTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "virtual-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "virtual-test pass != true"; exit 1 }
Write-Output "virtual-test offline PASS"
exit 0
