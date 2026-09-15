param([string]$PanelTest)
if (-not $PanelTest) { Write-Error "Missing -PanelTest"; exit 1 }
if (-not (Test-Path $PanelTest)) { Write-Error "Not found: $PanelTest"; exit 1 }
$out = & $PanelTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "panel-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "panel-test pass != true"; exit 1 }
Write-Output "panel-test offline PASS"
exit 0
