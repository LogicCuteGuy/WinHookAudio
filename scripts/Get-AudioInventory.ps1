# Read-only registry inventory. Does not open streams or change configuration.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskWarnings = [System.Collections.Generic.List[string]]::new()
$taskAsio = foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
    $baseKey = $null
    $asioKey = $null
    try {
        $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
        $asioKey = $baseKey.OpenSubKey('SOFTWARE\ASIO')
        if ($null -ne $asioKey) {
            foreach ($driverName in $asioKey.GetSubKeyNames()) {
                [pscustomobject]@{ Name = $driverName; RegistryView = $view.ToString(); StreamVerified = $false }
            }
        }
    } catch {
        $taskWarnings.Add("ASIO $view enumeration failed: $($_.Exception.Message)")
    } finally {
        if ($null -ne $asioKey) { $asioKey.Dispose() }
        if ($null -ne $baseKey) { $baseKey.Dispose() }
    }
}

$taskEndpoints = foreach ($direction in @('Render', 'Capture')) {
    $taskPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$direction"
    try {
        foreach ($endpoint in Get-ChildItem -LiteralPath $taskPath) {
            try {
                $values = Get-ItemProperty -LiteralPath $endpoint.PSPath
                $props = Get-ItemProperty -LiteralPath (Join-Path $endpoint.PSPath 'Properties')
                [pscustomobject]@{
                    Id = $endpoint.PSChildName
                    Direction = $direction
                    Description = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
                    RawRegistryState = $values.DeviceState
                    StreamVerified = $false
                }
            } catch {
                $taskWarnings.Add("Endpoint $($endpoint.PSChildName) read failed: $($_.Exception.Message)")
            }
        }
    } catch {
        $taskWarnings.Add("$direction enumeration failed: $($_.Exception.Message)")
    }
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    OsVersion = [System.Environment]::OSVersion.VersionString
    Method = 'Read-only registry snapshot; not a live MMDevice/KS capability probe'
    AsioDrivers = @($taskAsio)
    Endpoints = @($taskEndpoints)
    Warnings = @($taskWarnings.ToArray())
} | ConvertTo-Json -Depth 5
