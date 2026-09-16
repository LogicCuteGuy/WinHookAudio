# Spec — v10.4 WHAA Network (PCM + Vorbis + NETWORK 8)

## Problem Statement
From the user's perspective: `v10.3` `Control Panel` is green offline (`6/6` `ctest` PASS, `stream_verified:false` offline, `ImGui` model only) but has no `WHAA Network` — `WHAPacket.h` + `WHASlotTable` `netTx[8]`/`netRx[8]` + `WHAA CODEBOOK` already exist offline, `Worker` `MasterHolder` already has `Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` `Per-Thing` `FIFOs` but no `UDP 6980` `sendto`/`recvfrom`/`jitter`/`decode`/`SRC`, no `NETWORK 8` tab `Tx1..8`/`Rx1..8` `192.168.1.50:6980` `PCM_F32`/`VORBIS` `Q0.4` `Ch32` `BW`, no `libvorbis 1.3.7` + `libogg 1.3.5` + `r8brain-free` `SRC` if remote `SR` differs, no `6981` `WHAA_HELLO 2s` + `netsh 6980-6981`. No `Virtual Cable`, no `Installer` in this spec.

## Solution
From the user's perspective: `WHAA Network` `PCM_F32`/`PCM_I16`/`VORBIS Q0.1–1.0` + `CODEBOOK` handshake + `jitter 20/50ms` + `r8brain SRC` if remote `SR` differs, `UDP 6980` audio + `6981` discovery `WHAA_HELLO 2s` deferred, `NETWORK 8` tab `Tx1..8`/`Rx1..8` `192.168.1.50:6980` `PCM_F32`/`VORBIS` `Q0.4` `Ch32` `BW` in first slice, `MasterHolder::doTick()` does `Tx: SHM Out → PCM payload → sendto 6980` + `Rx: recvfrom nonblock → jitter 20ms → pop on Master_Tick → SHM In`, one `MMCSS Pro Audio` thread, `jitter` as `std::queue` per `streamId 0..7`, `libvorbis 1.3.7 + libogg 1.3.5 + r8brain-free` vendored under `third_party/libvorbis/` + `third_party/r8brain/` like `ASIO`/`ImGui`, `.gitignore` payload, `README` fetch step, stub `Vorbis` as `PCM` fallback for offline `ctest` (`stream_verified:false`), `6981 WHAA_HELLO 2s` + `netsh 6980-6981` deferred to keep offline gate without `ADMIN`.

Seam: `WHAPacket.h` (`WHAA` `Header` + `CODEBOOK`) + `wha_common` (`WHASlotTable` `netTx[8]`/`netRx[8]` + `WHASharedMemory` `kSlotsJsonPath`) + `MasterHolder` (`doTick` `Tx`/`Rx` + `jitter` + `SRC`) + `WHAControlPanelUI.cpp` (`NETWORK 8` tab `Tx1..8`/`Rx1..8` `Per-stream` `codec` `Q` `Ch` `BW`) + `KsAudio` (`WASAPI` `hwBuffer 64` already done). Offline gate: `PCM` `UDP 127.0.0.1:6980` `sendto`/`recvfrom` + `jitter 20ms` + `CODEBOOK 3×` on `Save` without `Vorbis` encode, `stream_verified:false` without `ADMIN`/firewall. `Vorbis Q0.1–1.0` + `libvorbis` + `r8brain` follow as `15` with `third_party/libvorbis/`.

## User Stories
1. As a network user, I want `NETWORK 8` tab `Tx1 [192.168.1.50:6980] [PCM_F32 v]/[PCM_I16]/[VORBIS] Q[0.4 v] Ch[32 v] BW 6.1 Mbps [v]` + `Rx1 [192.168.1.50 v] [VORBIS v] Q0.4 Ch2 192kbps [v] → linked to INPUTS where Type=NETWORK Rx1`, so that `Per-stream` `codec` `Q` `Ch` `BW` is visible.
2. As a network user, I want `PCM_F32` `0 ms` `LAN` + `PCM_I16` `half BW` + `VORBIS` `30–50 ms` `WAN` `Q0.1–1.0` `64–500 kbps` per stereo `Per-stream`, so that `64ch` fits `WAN` bandwidth.
3. As a network user, I want `Worker Tx: SHM Out → encode (Vorbis needs 256 frames) → sendto 6980` + `Worker Rx: recvfrom nonblock → jitter PCM 20ms / Vorbis 50ms → decode → pop on Master_Tick`, so that `PCM` `~23ms` `LAN`, `Vorbis` `~58ms` `WAN` `Worker` does `r8brain SRC` if remote `SR` differs.
4. As a network user, I want `Header: magic WHAA v5 codec PCM_F32/PCM_I16/VORBIS channels frames streamId sequence qpc payloadBytes + payload` + `Handshake: WHAA CODEBOOK streamId SR Ch quality headersBytes + 3 Vorbis headers (ident/comment/codebook)` `3×` `redundant` on `Save`, so that `Vorbis` `CODEBOOK` is `redundant` `3×`.
5. As a network user, I want `jitter PCM 20ms / Vorbis 50ms` `Per-Thing` `FIFOs` `Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` without `host reset` (Worker reloads via `TableChanged`), so that `PCM` stays tight and `Vorbis` stays stable.
6. As a network user, I want `UDP 6980` audio `127.0.0.1` loopback without `firewall`/`ADMIN`, so that offline `ctest` proves `sendto`/`recvfrom` + `jitter` without `netsh`.
7. As a network user, I want `Vorbis Q0.1–1.0` `64–500 kbps` per stereo `Per-stream` `codec` `Q` in `NETWORK` tab, so that `Q0.4` `~1.2 Mbps` compressed `8` streams can carry `256ch` `PCM` or `512ch` `Vorbis`.
8. As a performer, I want `Master Clock` `2.7ms@128/48k` `ONE tick for all 512ch` with `Worker` `MMCSS Pro Audio` `WaitForSingleObject(Master_Tick)`, so that `jitter 20ms` is `~7` ticks `128/48k`.
9. As a tester, I want offline `ctest` green without `ADMIN`/firewall/`sys`/`DAW`, `stream_verified:false`, `PCM` `sendto`/`recvfrom` + `jitter` `20ms` + `CODEBOOK` `3×` verified via `host-sample` stub.
10. As a developer, I want `libvorbis 1.3.7 + libogg 1.3.5 + r8brain-free` vendored under `third_party/libvorbis/` + `third_party/r8brain/` like `ASIO`/`ImGui`, `.gitignore` payload, `README` fetch step, stub `Vorbis` as `PCM` fallback for offline `ctest`.

## Implementation Decisions
- `WHAA` `PCM_F32`/`PCM_I16`/`VORBIS Q0.1–1.0` + `CODEBOOK` handshake + `jitter 20/50ms` + `r8brain SRC` if remote `SR` differs, `UDP 6980` audio + `6981` discovery `WHAA_HELLO 2s` deferred, `NETWORK 8` tab `Tx1..8`/`Rx1..8` `192.168.1.50:6980` `PCM_F32`/`VORBIS` `Q0.4` `Ch32` `BW` in first slice, `MasterHolder::doTick()` does `Tx: SHM Out → PCM payload → sendto 6980` + `Rx: recvfrom nonblock → jitter 20ms → pop on Master_Tick → SHM In`, one `MMCSS Pro Audio` thread, `jitter` as `std::queue` per `streamId 0..7`, `libvorbis 1.3.7 + libogg 1.3.5 + r8brain-free` vendored under `third_party/libvorbis/` + `third_party/r8brain/` like `ASIO`/`ImGui`, `.gitignore` payload, `README` fetch step, stub `Vorbis` as `PCM` fallback for offline `ctest` (`stream_verified:false`), `6981 WHAA_HELLO 2s` + `netsh 6980-6981` deferred to keep offline gate without `ADMIN`.
- `WHAPacket.h` `WHAA Header` + `CODEBOOK` already exist offline, `WHASlotTable` `netTx[8]`/`netRx[8]` `WHANetworkStream` `ip[16]` `port 6980` `codec` `quality 0.4` `channels 2` already exist, `Worker` `MasterHolder` already has `Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` `Per-Thing` `FIFOs` but no `UDP` `sendto`/`recvfrom`/`jitter`/`decode`/`SRC`.
- `WHAControlPanelUI.cpp` one `ImGui` code — Master & Bridge share `NETWORK 8` tab `Tx1..8`/`Rx1..8` `Per-stream` `codec` `Q` `Ch` `BW`, `INPUTS` `Type=NETWORK` `Rx1` linkage.
- `C++20` user-mode, `MSVC` `x64` `MultiThreaded`, `/W4 /WX`, `wha_common` + `MasterHolder` + `WHAControlPanelUI` + `KsAudio`, `ImGui` `Win32`+`DX11` backends, `libvorbis` `Win32` + `r8brain` header-only.
- Vocabulary: `Master DAW`/`Slave DAW`/`Master Driver`/`Bridge Driver`/`Virtual Cable`/`Slot`/`Slot Table`/`Master Clock`/`Loopback`/`Shared Bridge`/`Network Stream`/`Control Panel`/`Worker` per `CONTEXT.md` + ADRs 0001–0009.

## Testing Decisions
- Good tests check external behavior only: `NETWORK 8` `Tx`/`Rx` `ip:port` `codec` `Q` `Ch` `BW`, `PCM` `sendto`/`recvfrom` + `jitter` `20ms` + `CODEBOOK` `3×`, `Vorbis` `Q0.1–1.0` stub, `MasterHolder` `doTick` `Tx`/`Rx` + `jitter` + `SRC` stub, not `ImGui` internals or `libvorbis` internals.
- Modules tested: `WHAPacket.h`, `wha_common` (`WHASlotTable` `netTx`/`netRx`), `MasterHolder` (`doTick` `Tx`/`Rx` + `jitter`), `WHAControlPanelUI.cpp` (`NETWORK 8` tab), `KsAudio` already done.
- Prior art: `tests/Abi-Check.ps1` + `tests/Probe-Cli.ps1` + `tests/Host-Sample.ps1` + `tests/Worker-Test.ps1` + `tests/Ks-Test.ps1` + `tests/Panel-Test.ps1` offline pattern; extend with `WHAA` `PCM` `sendto`/`recvfrom` + `jitter` + `CODEBOOK` checks.
- All offline tests run without `ADMIN`/firewall/`sys`/`DAW`/device/network, `stream_verified:false`, `UDP` `127.0.0.1:6980` loopback.
- Failure cases: `WHAA` invalid `codec`/`channels`/`streamId`/`payload` still rejected offline, `masterInCount`/`masterOutCount` `0`/`513`, `loopback` on non-`VIRTUAL`, `5th Bridge client` `ASE_NotPresent`.

## Out of Scope
- `6981` `WHAA_HELLO 2s` broadcast + `netsh advfirewall firewall add rule name="WinHookAudio" dir=in action=allow protocol=UDP localport=6980-6981` + `ADMIN=True` + `Bitwig` runtime gate — deferred to keep offline gate without `ADMIN`.
- `Vorbis` `Q0.1–1.0` `64–500 kbps` per stereo `libvorbis 1.3.7` + `libogg 1.3.5` encode/decode + `r8brain-free` `SRC` if remote `SR` differs — stub as `PCM` fallback in first slice, full in `15`.
- `Virtual Cable` `WinHookAudio.sys` `8× Stereo` `IOCTL_WHA_READ/WRITE`/`SET_LOOPBACK` `64KB RingBuffer` `WDK` `PortCls` `WaveRT`.
- `Installer` `5 CLSIDs` + `WinHookAudio.reg` + `Inno Setup` + firewall `6980-6981` + `EV Cert`/`Attestation`.

## Further Notes
- Respects ADRs 0001–0009: `512` pool, Pure DLL Worker, `WHAA` only, Shared sum, Opt-in Loopback, Five-tab panel, `ASIO SDK` vendoring, `Control Panel` `ImGui DX11`, `WHAA Vorbis` vendoring.
- `v10.4` governs over `v10.3` where they conflict; `v10.3` `11`→`13` remain green as offline gate.
- No audio claim without runtime verification (`stream_verified:true` only after `UDP` `127.0.0.1:6980` `PCM` measured).
