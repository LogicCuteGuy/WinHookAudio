param([string]$InstallerTest)
if (-not $InstallerTest) { Write-Error "Missing -InstallerTest"; exit 1 }
if (-not (Test-Path $InstallerTest)) { Write-Error "Not found: $InstallerTest"; exit 1 }
$out = & $InstallerTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "installer-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "installer-test pass != true"; exit 1 }
Write-Output "installer-test offline PASS"
exit 0
