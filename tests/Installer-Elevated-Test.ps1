param([string]$InstallerElevatedTest)
if (-not $InstallerElevatedTest) { Write-Error "Missing -InstallerElevatedTest"; exit 1 }
if (-not (Test-Path $InstallerElevatedTest)) { Write-Error "Not found: $InstallerElevatedTest"; exit 1 }
$out = & $InstallerElevatedTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "installer-elevated-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "installer-elevated-test pass != true"; exit 1 }
Write-Output "installer-elevated-test offline PASS"
exit 0
