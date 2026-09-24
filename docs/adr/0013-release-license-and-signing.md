# MIT release, installer driver choice and signing

Date: 2026-09-24
Status: accepted (supersedes the driver and test-signing parts of 0011; release part of 0007)

## Context
The project goes public under the MIT License with GitHub Sponsors and code signing from SignPath
Foundation (applied for). Facts that shape the design:

- Windows 10 1607+ / 11 load a kernel driver only when Microsoft signed it (attestation signing in
  Partner Center, which needs an EV certificate). SignPath signs user-mode files; a SignPath
  signature does not let `WinHookAudio.sys` load without Test Mode.
- SignPath Foundation requires an OSI license and no proprietary or commercially dual-licensed
  component. The Steinberg ASIO SDK 2.3.4 is dual-licensed (Steinberg license or GPLv3).
- `pnputil /add-driver /install` (0011) only stages a package. `Root\WinHookAudio` is
  root-enumerated, so its device node must be created (what `devcon install` does); devcon is not
  redistributable.
- The third-party libraries are ignored by git (0007, 0008, 0009), so a fresh checkout has to fetch them.

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
- **Build**: local builds follow the `README.md` steps (`scripts/Fetch-ThirdParty.ps1` with pinned
  tag + commit, CMake, offline tests, `driver/build.cmd`, which finds the newest VS and WDK, then
  `installer/build-installer.ps1`). The GitHub Actions workflow was removed on 2026-09-24 and added
  back the same day for the SignPath application, since SignPath accepts only GitHub Actions or
  GitLab CI builds. It builds, tests and packages without the test-signed driver, because SignPath
  does not sign software that turns on Test Mode; the test-signed setup stays a local build.

## Consequences
- Until a Microsoft-signed package exists, users of the Virtual Cable need Test Mode and Secure
  Boot off. The ASIO drivers, HW devices, Bridges and network work without it.
- The test certificate is the one in the builder's certificate store; setup trusts the one it ships.
- SignPath Foundation signs only CI-built files, so only the GitHub Actions setup (no Virtual
  Cable until a Microsoft-signed driver exists) can be SignPath-signed.
- Getting the signed driver needs an EV certificate and a Partner Center account;
  every driver change needs a new submission.
- `common/WHAAsio.h` must follow the SDK's declarations by hand; its `static_assert`s pin the
  struct sizes (SDK pack 4).
