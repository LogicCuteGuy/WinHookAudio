# Spec — v10.3 Control Panel (ImGui DX11 Popup Type 1)

## Problem Statement
From the user's perspective: `v10.2` Master Driver is green offline (`5/5` `ctest` PASS, `stream_verified:false` offline, `true` only after measured tone) but has no UI to edit `512 In + 512 Out` `Slot Table` — `+Add Input`, `Loop` checkbox, `Bridge` counts, `Per-Thing` `GENERAL`, and `slots.json` persistence require `Control Panel` `INPUTS 512 | OUTPUTS 512 | GENERAL | ABOUT` with `ListClipper` `512` rows `20` drawn, `Filter`/`Search`, `+Add`/`+Insert Empty`/`Duplicate`/`X`/`::` drag, `Loop` (`VIRTUAL` only), `En`, and full `Save` contract (`memcpy SHM` + `version++` + `slots.json` + `FlushViewOfFile` + `SetEvent(TableChanged)` + `hostCallback(ASIOResetRequest)`). No `NETWORK` tab, no `Virtual Cable`, no `WHAA Network`, no `Installer` in this spec.

## Solution
From the user's perspective: `Control Panel` Popup Type 1 (`controlPanel()` → `CreateThread` → `ImGui DX11` `1180×720` `TopMost` `dark #1E1E1E`) with `WHAControlPanelUI.cpp` one `ImGui` code for Master & Bridge (Bridge filtered to `BRIDGE1` only, `GENERAL` read-only `Follows Master`), `INPUTS 512` + `OUTPUTS 512` + `GENERAL` + `ABOUT` tabs, `ListClipper` `512` rows `20` drawn, `Filter:[All/HW/VIRTUAL/NETWORK/BRIDGE1..4]` `Search`, `+Add`/`+Insert Empty Above/Below`/`Duplicate`/`X`/`::` drag `memmove` `INPUT` only, `Loop` checkbox (`VIRTUAL` only, grey otherwise), `En` `X`, `GENERAL` `Master Clock` `48000`/`32 Float`/`128` `2.7ms` + `Per-Thing` `HW 64`/`Virtual 256`/`Bridge1..4 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` + `MMCSS`/`Exclusive`/`Virtual Cables 8`/`Ports 6980/6981`/`Perf`, `ABOUT` `Version`/`sys`/`5 CLSIDs`/`slots.json`/`Export/Import`/`Firewall`/`Reset`/`Bridge clients 2/4`, full `Save` contract.

Seam: `WHAControlPanelUI.cpp` (`ImGui` `Win32`+`DX11` backends, `ListClipper`) + `wha_common` (`WHASlotTable`/`WHASharedMemory`/`WHASlotsJson`) + `IASIO` (`controlPanel()` + `hostCallback(ASIOResetRequest)`). Offline gate: `ImGui` popup without `sys`/`UDP`, `slots.json` round-trip, `version++`, `TableChanged`, `ASIOResetRequest`, `ctest` green without admin/DAW. `ImGui 1.90` vendored under `third_party/imgui/` per ADR 0008, `.gitignore` payload, stub for offline build.

## User Stories
1. As a Bitwig user, I want `Preferences → Audio → ASIO → WinHookAudio Master → Configuration` to open `Control Panel` `1180×720` `TopMost` `dark #1E1E1E`, so that I can edit `512` slots without leaving the DAW.
2. As a template keeper, I want `INPUTS 512` with `ListClipper` `512` rows `20` drawn, `Filter:[All/HW/VIRTUAL/NETWORK/BRIDGE1..4]` `Search`, so that `512` rows stay filterable.
3. As a template keeper, I want `::` drag `memmove` `INPUT` only, `+Insert Empty Above/Below`, `+Add`, `X` delete, `Duplicate`, so that adding a bass mic does not shift guitars and `In04` does not move `Out04`.
4. As a streamer, I want `Loop` checkbox per `VIRTUAL` slot (`VIRTUAL` only, grey otherwise), so that `VRChat → Virtual1 → DAW → Virtual3 LOOP → VRCT` `~5.4ms` loop is toggled per slot.
5. As a Slave DAW user, I want `Bridge1:2/4` badge and `ABOUT` `Bridge clients: Bridge1 2/4`, so that `FL + Live` shared `16ch` is visible.
6. As a performer, I want `GENERAL` `Master Clock` `48000`/`32 Float`/`128` `2.7ms` `ONE tick for all 512ch` with `Save & Reset DAW → ASIOResetRequest`, so that `Master Clock` change re-queries `getChannels`/`getChannelInfo`.
7. As a performer, I want `GENERAL` `Per-Thing` `HW 64`/`Virtual 256`/`Bridge1..4 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` without `host reset` (Worker reloads), so that `HW 64` stays low-latency while `Network` stays buffered.
8. As a template keeper, I want `Save` → `editCopy → *pTable, version++, WriteFile slots.json, FlushViewOfFile, SetEvent(TableChanged), hostCallback(ASIOResetRequest) → DAW re-calls getChannels/getChannelInfo → "- empty -" grey, loopback active, new bridge clients re-count`, so that reboot restores the full map.
9. As a Bridge user, I want `Bridge` popup (`FL Studio → Bridge 1 → Configuration`) filtered to `BRIDGE1` only, `GENERAL` read-only `Follows Master 48000/32/128`, so that Slave DAWs cannot split the clock.
10. As a tester, I want `ABOUT` `Version`, `WinHookAudio.sys` running, `5 CLSIDs`, `%ProgramData%\WinHookAudio\slots.json`, `Export/Import`, `Firewall UDP 6980-6981 [Fix]`, `Reset Default`, so that install is verifiable offline.
11. As a developer, I want `WHAControlPanelUI.cpp` one `ImGui` code for Master & Bridge, so that `Master` and `Bridge` share `ListClipper`/`Filter`/`Loop` logic.
12. As a tester, I want offline `ctest` green without admin/DAW/device/network, `stream_verified:false`, `slots.json` round-trip, `version++`, `TableChanged`, `ASIOResetRequest` verified via `host-sample` stub.

## Implementation Decisions
- `ImGui 1.90 + Win32 + DX11` per datasheet FINAL v10 §8, vendored under `third_party/imgui/` per ADR 0008, `.gitignore` payload, document fetch step, stub for offline build (no `ImGui` required for `ctest`).
- `WHAControlPanelUI.cpp` one `ImGui` code — Master & Bridge share `ListClipper` (`512` rows `→ 20` drawn), `Filter`/`Search`, `Loop` checkbox, `En`, `X`, `::` drag `memmove`, `+Add`/`+Insert Empty`/`Duplicate`.
- `INPUTS 512` (`masterIn[512]` only) + `OUTPUTS 512` (`masterOut[512]` only, independent indices) + `GENERAL` (`Per-Thing`, `GLOBAL`, not per slot) + `ABOUT` (no `NETWORK` — deferred with `WHAA` `libvorbis`/`UDP`).
- `controlPanel()` → `CreateThread` → `PopupThread` `ImGui DX11` `1180×720` `TopMost` `dark #1E1E1E`, `copy to editCopy` → `ImGui` loop with `ListClipper` → `Save` → `memcpy SHM` + `slots.json` + `hostCallback(ASIOResetRequest)`.
- `Save` full contract: `editCopy → *pTable, version++, WriteFile %ProgramData%\WinHookAudio\slots.json, FlushViewOfFile, SetEvent(TableChanged), hostCallback(ASIOResetRequest) → DAW re-calls getChannels/getChannelInfo`. `Per-Thing` `FIFO` change reloads `Worker` without `host reset` (via `TableChanged`).
- `Bridge` popup filtered to `BRIDGE1` only, `GENERAL` read-only `Follows Master`.
- `C++20` user-mode, `MSVC` `x64` `MultiThreaded`, `/W4 /WX`, `wha_common` + `IASIO` (`ASIOStub.h` or real `SDK`), `ImGui` `Win32`+`DX11` backends.
- Vocabulary: `Master DAW`/`Slave DAW`/`Master Driver`/`Bridge Driver`/`Virtual Cable`/`Slot`/`Slot Table`/`Master Clock`/`Loopback`/`Shared Bridge`/`Network Stream`/`Control Panel`/`Worker` per `CONTEXT.md` + ADRs 0001–0008.

## Testing Decisions
- Good tests check external behavior only: `ListClipper` `512` rows, `Filter`/`Search`, `+Add`/`Loop`/`En`, `Per-Thing` `GENERAL`, `ABOUT`, `slots.json` round-trip, `version++`, `TableChanged`, `ASIOResetRequest`, `host-sample` stub, not `ImGui` internals.
- Modules tested: `WHAControlPanelUI.cpp`, `wha_common` (`WHASlotTable`/`WHASlotsJson`), `IASIO` (`controlPanel` + `hostCallback`).
- Prior art: `tests/Abi-Check.ps1` + `tests/Probe-Cli.ps1` + `tests/Host-Sample.ps1` + `tests/Worker-Test.ps1` + `tests/Ks-Test.ps1` offline pattern; extend with `Control Panel` `slots.json` + `version` + `TableChanged` checks.
- All offline tests run without admin, DAW, device, network, `stream_verified:false`.
- Failure cases: `masterInCount`/`masterOutCount` `0`/`513`, overlong `name[32]`, `loopback` on non-`VIRTUAL`, `5th Bridge client` `ASE_NotPresent`, `WHAA` invalid `codec`/`channels`/`streamId`/`payload` still rejected offline.

## Out of Scope
- `NETWORK 8` tab (`Tx1..8`/`Rx1..8` `192.168.1.50:6980` `PCM_F32`/`PCM_I16`/`VORBIS` `Q0.4` `Ch32` `BW`, `Discovery 6981`, `CODEBOOK` handshake) — deferred with `WHAA Network` `UDP 6980/6981` `libvorbis` `jitter` `r8brain SRC`.
- `Virtual Cable` `WinHookAudio.sys` `8× Stereo` `IOCTL_WHA_READ/WRITE`/`SET_LOOPBACK` `64KB RingBuffer`.
- `WHAA Network` `UDP 6980/6981` `PCM_F32`/`PCM_I16`/`VORBIS Q0.1–1.0` + `CODEBOOK` handshake + `jitter` + `r8brain SRC` + `discovery`.
- `Installer` `5 CLSIDs` + `WinHookAudio.reg` + `Inno Setup` + firewall `6980-6981` + `EV Cert`/`Attestation`.

## Further Notes
- Respects ADRs 0001–0008: `512` pool, Pure DLL Worker, `WHAA` only, Shared sum, Opt-in Loopback, Five-tab panel, `ASIO SDK` vendoring, `Control Panel` `ImGui DX11`.
- `v10.3` governs over `v10.2` where they conflict; `v10.2` `07`→`10` remain green as offline gate.
- No audio claim without runtime verification (`stream_verified:true` only after Bitwig + `KS` measured).
