# install.ps1 - installs (or removes) the test-signed WinHookAudio Virtual Cable driver from
# build\driver on THIS test machine. Run in an Administrator PowerShell. Needs test-signing mode
# (bcdedit /set testsigning on, then a restart) and driver\build.cmd run first.
#
#   install.ps1              trust the test certificate, create or update the Root\WinHookAudio device
#   install.ps1 -Uninstall   remove the device, the driver package and the certificate trust
#
# What it changes on the machine:
#   - LocalMachine\Root and LocalMachine\TrustedPublisher: the "WinHookAudio Test" certificate
#     (so Windows accepts the test-signed package without a prompt). -Uninstall removes it.
#   - A root-enumerated device Root\WinHookAudio with its driver (devcon), which adds the Windows
#     endpoints "WinHookAudio Virtual 1" (playback and recording).
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this in an Administrator PowerShell.'
}
$package = Join-Path $PSScriptRoot '..\build\driver' | Resolve-Path -ErrorAction SilentlyContinue
$devcon = 'C:\Program Files (x86)\Windows Kits\10\Tools\10.0.28000.0\x64\devcon.exe'
if (-not (Test-Path $devcon)) { throw "devcon not found: $devcon" }
$hardwareId = 'Root\WinHookAudio'
$certSubject = 'CN=WinHookAudio Test'

function Get-OurDriverPackages {
    # Published names (oemNN.inf) of WinHookAudio.inf packages in the driver store.
    $lines = & pnputil.exe /enum-drivers
    $packages = @(); $published = $null
    foreach ($line in $lines) {
        if ($line -match '^\s*Published Name\s*:\s*(\S+)') { $published = $Matches[1] }
        elseif ($line -match '^\s*Original Name\s*:\s*winhookaudio\.inf\s*$' -and $published) { $packages += $published }
    }
    return $packages
}

if ($Uninstall) {
    Write-Host 'Removing the Root\WinHookAudio device...'
    & $devcon remove $hardwareId
    foreach ($p in Get-OurDriverPackages) {
        Write-Host "Removing driver package $p..."
        & pnputil.exe /delete-driver $p /uninstall /force
    }
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $certSubject } | ForEach-Object {
            Write-Host "Removing the test certificate from LocalMachine\$store"
            Remove-Item $_.PSPath
        }
    }
    Write-Host 'Done.'
    return
}

if (-not $package -or -not (Test-Path (Join-Path $package 'WinHookAudio.cat'))) {
    throw 'build\driver\WinHookAudio.cat not found: run driver\build.cmd first.'
}
$inf = Join-Path $package 'WinHookAudio.inf'
$cer = Join-Path $package 'WinHookAudioTest.cer'

$boot = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions
if ($boot -notmatch 'TESTSIGNING') { throw 'Test-signing mode is off: bcdedit /set testsigning on, then restart.' }

foreach ($store in 'Root', 'TrustedPublisher') {
    Write-Host "Trusting the test certificate in LocalMachine\$store"
    Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}

$existing = & $devcon find $hardwareId | Select-String -SimpleMatch 'ROOT\'
if ($existing) {
    Write-Host 'Updating the driver of the existing Root\WinHookAudio device...'
    & $devcon update $inf $hardwareId
} else {
    Write-Host 'Creating the Root\WinHookAudio device...'
    & $devcon install $inf $hardwareId
}
if ($LASTEXITCODE -ne 0) { throw "devcon failed (exit $LASTEXITCODE)" }
& $devcon status $hardwareId
Write-Host 'Done. Windows sound settings should now list "WinHookAudio Virtual 1" (playback and recording).'
