[CmdletBinding()]
param([Parameter(Mandatory)][string]$AbiCheck)

$ErrorActionPreference = 'Stop'
# Offline only: abi-check must not touch devices, DAW, driver, network, or files.
$output = & $AbiCheck 2>$null
$code = $LASTEXITCODE
if ($code -ne 0) { throw "abi-check exited with code $code" }
$result = ($output -join "`n") | ConvertFrom-Json
if ($result.operation -ne 'abi_check') { throw 'Missing operation abi_check' }
if ($result.schema_version -ne 1) { throw 'Bad schema_version' }
if ($result.stream_verified -ne $false) { throw 'stream_verified must be false offline' }
if ($result.pass -ne $true) { throw "abi-check did not pass: $($output -join "`n")" }
$required = @('counts_1_to_512', 'empty_counts_silent', 'name_truncation_32', 'loopback_virtual_only', 'bridge_inside_pool', 'network_streams_8', 'master_clock_defaults', 'shm_names_unique', 'shm_sizes_fixed')
foreach ($name in $required) {
    $check = $result.checks | Where-Object { $_.name -eq $name }
    if (-not $check) { throw "Missing check $name" }
    if ($check.pass -ne $true) { throw "Check $name did not pass" }
}
Write-Output "Passed abi-check with $($result.checks.Count) offline checks. No devices queried."
