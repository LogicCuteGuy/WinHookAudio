# MIT release, installer driver choice, CI and signing

Date: 2026-09-24
Status: accepted (supersedes the driver and test-signing parts of 0011; release part of 0007)

## Context
The project goes public under the MIT License with GitHub Actions builds, GitHub Sponsors and code
signing from SignPath Foundation (applied for). Facts that shape the design:

- Windows 10 1607+ / 11 load a kernel driver only when Microsoft signed it (attestation signing in
  Partner Center, which needs an EV certificate). SignPath signs user-mode files; a SignPath
  signature does not let `WinHookAudio.sys` load without Test Mode.
- SignPath Foundation requires an OSI license and no proprietary or commercially dual-licensed
  component. The Steinberg ASIO SDK 2.3.4 is dual-licensed (Steinberg license or GPLv3).
- `pnputil /add-driver /install` (0011) only stages a package. `Root\WinHookAudio` is
  root-enumerated, so its device node must be created (what `devcon install` does); devcon is not
  redistributable.
- The third-party libraries are ignored by git (0007, 0008, 0009), so CI has to fetch them.

## Decision
- **License**: MIT for this repository's code; `THIRD-PARTY-NOTICES.md` lists ImGui (MIT),
  libogg / libvorbis (BSD-3-Clause) and r8brain (MIT).
- **ASIO**: release builds compile against `common/WHAAsio.h` (the project's own declaration of
  the ASIO interface), never the Steinberg SDK. The SDK stays optional for local builds.
- **Installer** (`installer/WinHookAudio.iss`): the Components page offers the Virtual Cable as
  exclusive choices, **Signed driver** (Microsoft-signed package from `installer/driver-signed/`,
  listed only when present) or **Not signed: test-signed driver** (trust the test certificate,
  `bcdedit /set testsigning on` when it was off, warn when Secure Boot is on, ask for a restart),
  or no cable. `winhookaudio-devsetup.exe` (`installer/devsetup/`) creates, updates and removes the
  device with SetupAPI / newdev. Uninstall turns Test Mode off only when setup turned it on.
- **CI** (`.github/workflows/build.yml`, windows-2025 with VS 2022, WDK 10.0.26100, Inno Setup):
  fetch pinned libraries (`scripts/Fetch-ThirdParty.ps1`, tag + commit hash), build, offline
  tests except `network-offline`, test-signed driver (`driver/build.cmd` now finds the newest VS
  and WDK), installer. A `v*` tag publishes a GitHub Release. SignPath signing runs only when the
  `SIGNPATH_*` repository variables and secret are set (`docs/signing.md`).

## Consequences
- Until a Microsoft-signed package exists, users of the Virtual Cable need Test Mode and Secure
  Boot off. The ASIO drivers, HW devices, Bridges and network work without it.
- Each CI run makes a new self-signed test certificate; setup trusts the one it ships.
- Getting the signed driver needs an EV certificate and a Partner Center account, outside CI;
  every driver change needs a new submission.
- `common/WHAAsio.h` must follow the SDK's declarations by hand; its `static_assert`s pin the
  struct sizes (SDK pack 4).
