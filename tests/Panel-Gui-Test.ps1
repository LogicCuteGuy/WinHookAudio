param([string]$PanelGuiTest)
if (-not $PanelGuiTest) { Write-Error "Missing -PanelGuiTest"; exit 1 }
if (-not (Test-Path $PanelGuiTest)) { Write-Error "Not found: $PanelGuiTest"; exit 1 }
$out = & $PanelGuiTest 2>&1 | Out-String
Write-Output $out
if ($LASTEXITCODE -ne 0) { Write-Error "panel-gui-test failed exit $LASTEXITCODE"; exit 1 }
if ($out -notmatch '"pass":true') { Write-Error "panel-gui-test pass != true"; exit 1 }
Write-Output "panel-gui-test offline PASS"
exit 0
