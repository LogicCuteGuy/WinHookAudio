# Installer Inno Setup + pnputil + Firewall

Date: 2026-09-16
Status: accepted, partly superseded by 0013 (driver install via winhookaudio-devsetup, signed / test-signed choice)

## Context
Installer needs `5 CLSIDs` `WinHookAudio Master` + `Bridge1..4` `HKLM\SOFTWARE\ASIO` via `WinHookAudio.reg` + `Inno Setup` `WinHookAudio.iss` + `WinHookAudio.sys` `8× Stereo` `WDK` `PortCls` `WaveRT` `WinHookAudio.inf/.cat` `Root\WinHookAudio` + `firewall` `netsh advfirewall firewall add rule name="WinHookAudio" dir=in action=allow protocol=UDP localport=6980-6981` + `EV Cert`/`Attestation` + `bcdedit` `testsigning`. `v10.5` `WinHookAudio.sys` is stub (`memcpy` fallback, no `sys` install). `ADR 0007` (`ASIO SDK`) + `ADR 0008` (`ImGui`) + `ADR 0009` (`libvorbis`/`r8brain`) + `ADR 0010` (`WDK`) all use `third_party/` + `.gitignore` payload + `README` fetch step + stub for offline build.

## Decision
Use `Inno Setup 6.x` `WinHookAudio.iss` with `[Setup]` `AppName` `DefaultDirName` `{pf}\WinHookAudio` + `[Files]` `WinHookAudioMasterASIO64.dll` + `WinHookAudioBridgeASIO64.dll` + `WinHookAudio.sys` + `WinHookAudio.inf/.cat` to `{app}\driver` + `[Registry]` `Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master/Bridge1..4"; ValueType: string; ValueName: "CLSID"; ValueData: "{CLSID_Master/B1..B4}"` + `[Run]` `pnputil /add-driver "{app}\driver\WinHookAudio.inf" /install` + `bcdedit /set testsigning on` for offline `test` cert + `[Run]` `netsh advfirewall firewall add rule name="WinHookAudio" dir=in action=allow protocol=UDP localport=6980-6981` with `ADMIN` check. `WinHookAudio.reg` `5` entries for offline `reg` syntax `ctest` without `ADMIN`, `WinHookAudio.inf` `Root\WinHookAudio` + `cat` placeholder for offline `inf` syntax, `EV Cert` + `Microsoft Attestation` for `Win11` normal mode deferred to `20` with `ADMIN=True` + `Bitwig` runtime gate. First slice is `reg`/`iss`/`inf` syntax offline (`stream_verified:false`) without `iscc`/`pnputil`/`netsh` run.

## Consequences
- `5 CLSIDs` `HKLM\SOFTWARE\ASIO` + `Inno` `iscc` + `WinHookAudio.inf/.cat` `Root\WinHookAudio` + `firewall` `6980-6981` proven via `reg`/`iss`/`inf` syntax `ctest` without `ADMIN`/`iscc`/`WDK`.
- `pnputil /add-driver` inbox `Win10+`, `Inno` `[Run]` `pnputil` with `testsigning` for offline `test` cert keeps `offline` `ctest` green without `EV Cert`.
- `EV Cert` + `Attestation` + `iscc` + `pnputil` + `netsh` + `ADMIN=True` + `Bitwig` runtime gate follow as `20`.
- `WinHookAudio.sys` `WDK` `PortCls` `WaveRT` `8× Stereo` `64KB` `RingBuffer` already stubbed in `v10.5` `17` as `memcpy` fallback.
