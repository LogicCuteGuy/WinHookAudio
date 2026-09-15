[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'results'))

$ErrorActionPreference = 'Stop'
$probeExecutable = Join-Path $PSScriptRoot 'bin/winhookaudio-probe.exe'
if (!(Test-Path -LiteralPath $probeExecutable -PathType Leaf)) { throw "Missing $probeExecutable" }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$resultPath = Join-Path $OutputDirectory ('discovery-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.json')
$resultText = & $probeExecutable discover
$resultCode = $LASTEXITCODE
$resultText | Set-Content -LiteralPath $resultPath -Encoding UTF8
if ($resultCode -ne 0) { throw "Probe exited $resultCode. See $resultPath" }
$inventory = ($resultText -join "`n") | ConvertFrom-Json
Write-Output "Saved $resultPath"
Write-Output "Inspect the JSON errors and evidence fields; discovery does not verify audio."
$inventory.mmdevice.endpoints | Where-Object state -eq 1 |
    Select-Object name, direction, property_queries | Format-Table -AutoSize
