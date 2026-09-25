# build-installer.ps1 - builds build\installer\WinHookAudio-Setup-<version>.exe with Inno Setup.
#
#   installer\build-installer.ps1 [-Version 1.2.3] [-BuildDir build\Release] [-TestDriverDir build\driver]
#                                 [-SignedDriverDir installer\driver-signed] [-BuildDirX86 build-x86\Release]
#                                 [-BuildDirArm64 build-arm64\Release]
# x64 is required; the 32-bit DLLs (cmake -A Win32 -B build-x86) and the ARM64 DLLs + ARM64X forwarders
# (cmake -A ARM64 -B build-arm64) go in when built (ADR 0016).
#
# Needs first: the CMake Release build (ASIO DLLs + winhookaudio-devsetup.exe). Optional:
#   - driver\build.cmd            -> test-signed Virtual Cable package (installer option "Not signed")
#   - installer\driver-signed\    -> Microsoft-signed package WinHookAudio.sys/.inf/.cat (option "Signed")
# A package that is missing is left out of the Components page. Changes nothing on this PC.
param(
    [string]$Version = '0.1.1',
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\Release'),
    [string]$BuildDirX86 = (Join-Path $PSScriptRoot '..\build-x86\Release'),
    [string]$BuildDirArm64 = (Join-Path $PSScriptRoot '..\build-arm64\Release'),
    [string]$TestDriverDir = (Join-Path $PSScriptRoot '..\build\driver'),
    [string]$SignedDriverDir = (Join-Path $PSScriptRoot 'driver-signed'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\build\installer')
)
$ErrorActionPreference = 'Stop'

$iscc = @(
    (Get-Command iscc.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found: install it from https://jrsoftware.org/isdl.php' }

foreach ($file in 'WinHookAudioMasterASIO64.dll', 'WinHookAudioBridgeASIO64.dll', 'winhookaudio-devsetup.exe') {
    if (-not (Test-Path (Join-Path $BuildDir $file))) { throw "$file not found in ${BuildDir}: build Release first." }
}
$full = { param($p) [IO.Path]::GetFullPath($p) }
$signed = Test-Path (Join-Path $SignedDriverDir 'WinHookAudio.sys')
$test = Test-Path (Join-Path $TestDriverDir 'WinHookAudio.sys')
Write-Host "Virtual Cable: signed package $(if ($signed) { 'yes' } else { 'no' }), test-signed package $(if ($test) { 'yes' } else { 'no' })"
$x86 = Test-Path (Join-Path $BuildDirX86 'WinHookAudioMasterASIO32.dll')
$arm64 = Test-Path (Join-Path $BuildDirArm64 'WinHookAudioMasterASIOARM64X.dll')
Write-Host "ASIO DLLs: x64 yes, 32-bit $(if ($x86) { 'yes' } else { 'no' }), ARM64 $(if ($arm64) { 'yes' } else { 'no' })"

& $iscc /Q "/DAppVersion=$Version" "/DBuildDir=$(& $full $BuildDir)" "/DBuildDirX86=$(& $full $BuildDirX86)" `
    "/DBuildDirArm64=$(& $full $BuildDirArm64)" "/DTestDriverDir=$(& $full $TestDriverDir)" `
    "/DSignedDriverDir=$(& $full $SignedDriverDir)" "/DOutputDir=$(& $full $OutputDir)" (Join-Path $PSScriptRoot 'WinHookAudio.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)" }
$setup = Join-Path (& $full $OutputDir) "WinHookAudio-Setup-$Version.exe"
Write-Host "Built $setup"
