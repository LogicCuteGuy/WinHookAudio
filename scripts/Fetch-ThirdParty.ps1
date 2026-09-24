# Fetch-ThirdParty.ps1 - downloads the open-source libraries the build uses into third_party/
# (ignored by git; see THIRD-PARTY-NOTICES.md). Each one is a pinned git tag, checked against its
# commit hash. A library that is already there is left alone (-Force fetches it again).
#
#   scripts\Fetch-ThirdParty.ps1 [-Destination third_party] [-Force]
#
# The Steinberg ASIO SDK is NOT fetched: release builds use common/WHAAsio.h (THIRD-PARTY-NOTICES.md).
param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\third_party'),
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

$libraries = @(
    @{ Name = 'imgui';     Repo = 'https://github.com/ocornut/imgui';            Tag = 'v1.90'
       Commit = 'b81bd7ed984ce095c20a059dd0f4d527e006998f'; Marker = 'backends\imgui_impl_dx11.cpp' },
    @{ Name = 'libogg';    Repo = 'https://github.com/xiph/ogg';                 Tag = 'v1.3.5'
       Commit = 'e1774cd77f471443541596e09078e78fdc342e4f'; Marker = 'include\ogg\ogg.h' },
    @{ Name = 'libvorbis'; Repo = 'https://github.com/xiph/vorbis';              Tag = 'v1.3.7'
       Commit = '0657aee69dec8508a0011f47f3b69d7538e9d262'; Marker = 'include\vorbis\codec.h' },
    @{ Name = 'r8brain';   Repo = 'https://github.com/avaneev/r8brain-free-src'; Tag = '7.5'
       Commit = '9e73d2dd59fd5b95108fdb4f590083e35758b45f'; Marker = 'CDSPResampler.h' }
)

New-Item -ItemType Directory -Force $Destination | Out-Null
foreach ($lib in $libraries) {
    $target = Join-Path $Destination $lib.Name
    if (-not $Force -and (Test-Path (Join-Path $target $lib.Marker))) {
        Write-Host "$($lib.Name): already there"
        continue
    }
    $temp = Join-Path ([IO.Path]::GetTempPath()) ("wha-fetch-" + [Guid]::NewGuid().ToString('N'))
    try {
        Write-Host "$($lib.Name): $($lib.Repo) $($lib.Tag)"
        & git -c advice.detachedHead=false clone --quiet --depth 1 --branch $lib.Tag $lib.Repo $temp
        if ($LASTEXITCODE -ne 0) { throw "git clone failed for $($lib.Name)" }
        $commit = (& git -C $temp rev-parse HEAD).Trim()
        if ($commit -ne $lib.Commit) { throw "$($lib.Name): tag $($lib.Tag) is $commit, expected $($lib.Commit)" }
        New-Item -ItemType Directory -Force $target | Out-Null
        # Keep this repository's own README.md in third_party/<name>/ (it documents the pin).
        Get-ChildItem -Force $temp | Where-Object { $_.Name -ne '.git' -and $_.Name -ne 'README.md' } |
            Copy-Item -Destination $target -Recurse -Force
        if (-not (Test-Path (Join-Path $target 'README.md')) -and (Test-Path (Join-Path $temp 'README.md'))) {
            Copy-Item (Join-Path $temp 'README.md') $target
        }
    } finally {
        if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
    }
}
Write-Host 'Third-party libraries ready.'
