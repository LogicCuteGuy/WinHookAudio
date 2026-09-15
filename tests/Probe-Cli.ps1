[CmdletBinding()]
param([Parameter(Mandatory)][string]$Probe)

$ErrorActionPreference = 'Stop'
# Offline only: every case is help or rejected before any Windows device API.
$helpText = & $Probe --help
if ($LASTEXITCODE -ne 0 -or ($helpText -join "`n") -notmatch 'discover') { throw 'Help failed' }

$baseOptions = @('open', '--filter', 'unused', '--pin', '0', '--rate', '48000',
    '--channels', '2', '--bits', '16', '--valid-bits', '16', '--channel-mask', '3',
    '--interface', 'looped', '--wave-format', 'extensible')
$cases = [System.Collections.Generic.List[object]]::new()
$cases.Add(@('unknown-command'))
$cases.Add(@('discover', 'unexpected'))
$cases.Add(@('open'))
$cases.Add(@('open', '--filter'))
$cases.Add(@($baseOptions + @('--pin', '1')))
$cases.Add(@($baseOptions + @('--unknown', 'value')))
foreach ($replacement in @(
    @('--rate', '-1'), @('--rate', '0'), @('--rate', '48000junk'),
    @('--rate', '4294967296'), @('--rate', '48000.0'), @('--rate', '800000'),
    @('--channels', '0'), @('--channels', '65'), @('--bits', '8'),
    @('--valid-bits', '17'), @('--valid-bits', '0'), @('--channel-mask', '1'),
    @('--interface', 'invalid'), @('--wave-format', 'float'), @('--wave-format', 'pcm')
)) {
    $arguments = $baseOptions.Clone()
    $index = [Array]::IndexOf($arguments, $replacement[0])
    $arguments[$index + 1] = $replacement[1]
    $cases.Add($arguments)
}
foreach ($arguments in $cases) {
    # Native stderr is usage text; stdout must remain one parseable JSON document.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $Probe @arguments 2>$null
    $code = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    $result = ($output -join "`n") | ConvertFrom-Json
    if ($code -ne 2 -or $result.operation -ne 'invalid_arguments' -or
        $result.schema_version -ne 1 -or $result.stream_verified -ne $false -or !$result.message) {
        throw "Invalid arguments were not rejected correctly: $($arguments -join ' ')"
    }
}
Write-Output "Passed help and $($cases.Count) offline argument rejection cases. No devices queried or opened."
