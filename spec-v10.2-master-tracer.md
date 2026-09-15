# Spec — v10.2 Master Tracer (Debt + Master Driver)

## Problem Statement
From the user's perspective: the v10.1 ABI slice is green offline (13/13 `abi-check`, 2/2 `ctest`, `678eee1` clean, `stream_verified:false`) but has debt that makes every future slice more expensive — `common/` is header-only via relative `../../common/`, 7+4+4 `#define` macros, `volatile int32_t clientCount` with `++` not `InterlockedIncrement`, duplicated `32/128` channel limits, and no central `static_assert(kSlotTableSize >= sizeof(WHASlotTable))`. The next audible step is Master Driver streaming: Bitwig must host `WinHookAudio Master` with `512 In + 512 Out` dynamic, stable indices, `"- empty -"` silence, and `hostCallback(ASIOResetRequest)` on `+Add Input`, then Worker `MMCSS` with per-thing FIFOs, Loopback `OUT→IN` next tick, and Shared Bridge `tanh` sum, then `KS` exclusive to Real HW. No Virtual Cable, no WHAA Network, no Control Panel, no Installer in this spec.

## Solution
From the user's perspective: first pay down debt in one PR (`07`) turning `common/` into a deep Slot Table module with one seam (`wha_common INTERFACE`, `constexpr`, `std::atomic`, single `32/128` source, central size assert), then three tracer tickets `08→10` each red-green per `/tdd`: `08` minimal `IASIO` (`getChannels`/`getChannelInfo`/`bufferSwitch` `memcpy` only, no Worker, no KS, proves DAW hosts DLL and 512 names appear), `09` Worker (`WaitForSingleObject(Master_Tick)`, per-thing FIFOs `HW 64`/`Virtual 256`/`Bridge 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms`, Loopback `OUT→IN` next tick, Bridge `tanh` sum + `3ms` timeout, `4× Bridge ×4 clients`), `10` `KS` exclusive `hwBuffer 64` to Real HW (audible tone, measured `512ch ×128×4×2=16MB` `memcpy ~0.05ms`).

Seam: `wha_common INTERFACE` (`common/WHASlotTable.h` + `WHASharedMemory.h` + `WHASlotsJson.h` + `WHABridgeShared.h` + `WHAPacket.h`) plus `IASIO` (`asio-master/` + `asio-bridge/`). Offline gate: compile-time asserts + `abi-check` + `ctest` without admin/DAW/device/network (`stream_verified:false`). Runtime gate: SDK `host/sample` then Bitwig in elevated new `cmd` (`ADMIN=True`), `stream_verified:true` only after measured tone.

## User Stories
1. As a developer, I want `common/` as `wha_common INTERFACE` with `target_include_directories`, so that `src/abi-check/main.cpp` uses `#include "wha/WHASlotTable.h"` not `../../common/`.
2. As a developer, I want `constexpr` not `#define` for `WHA_MAX`/`WHA_BRIDGE_COUNT`/`WHA_NAME_LEN`/`WHA_NET_STREAMS`/`WHA_BRIDGE_CLIENTS`/`WHA_BRIDGE_CHANNELS`/`WHA_BRIDGE_FRAMES`/`WHA_BRIDGE_BUFFERS`/`WHAA_MAGIC`/`WHAA_VERSION`, so that one source of truth prevents shotgun surgery.
3. As a developer, I want `std::atomic<int32_t> clientCount` with `fetch_add` not `volatile` + `++`, so that 5th client `ASE_NotPresent` is race-free.
4. As a developer, I want `IsValidNetworkChannels`/`IsValidWhaaChannels` single source in `WHASlotTable.h`, so that `32/128` limits change in one place.
5. As a developer, I want central `static_assert(kSlotTableSize >= sizeof(WHASlotTable))` and `kBridgeSharedSize == sizeof(WHABridgeShared)` (or derived `constexpr`), so that SHM size drift is caught at compile time.
6. As a Bitwig user, I want `WinHookAudio Master` to appear in `Preferences → Audio → ASIO`, so that I can select it as Master DAW.
7. As a Bitwig user, I want `getChannels(*in=masterInCount,*out=masterOutCount)` `1..512` dynamic, so that `+Add Input` grows the pool.
8. As a Bitwig user, I want `getChannelInfo` to return `"- empty -"` for `SLOT_NONE` and `name[32]` for others, so that template indices stay stable and grey.
9. As a Bitwig user, I want `hostCallback(ASIOResetRequest)` on `+Add Input`/`Save`, so that Bitwig re-queries `getChannels`/`getChannelInfo` without restart.
10. As a Bitwig user, I want `bufferSwitch` to do only `DAW In ← SHM In` + `SHM Out ← DAW Out` + `SetEvent(Master_Tick)` `memcpy`, so that real-time thread never blocks.
11. As a performer, I want Worker `MMCSS Pro Audio` `WaitForSingleObject(Master_Tick)` with per-thing FIFOs, so that `HW 64` stays low-latency while `Network 512/1024` stays buffered.
12. As a streamer, I want Loopback `OUT→IN` next tick for `VIRTUAL` slots with `loopback==true`, so that `VRChat → Virtual1 → DAW → Virtual3 LOOP → VRCT` is `~5.4ms` without extra track.
13. As a Slave DAW user, I want Shared Bridge `4× Bridge ×4 clients` summed with `tanh` soft-clip and `3ms` timeout, so that `FL + Live` at `-6dB` arrive loud not quiet, and lagging client is silenced that tick.
14. As a performer, I want `KS` exclusive `hwBuffer 64` to Real HW (Scarlett/Realtek) via `WASAPI Exclusive`/`KS`, so that DAW tone reaches hardware.
15. As a tester, I want SDK `host/sample` to `CoCreateInstance(CLSID_Master)` → `init` → `getChannels` → `getChannelInfo` → `createBuffers` → `start` → `bufferSwitch` loopback without DAW, so that `IASIO` contract is verified offline.
16. As a tester, I want Bitwig runtime gate in elevated new `cmd` to load `WinHookAudio Master`, show 512 names, and measure `memcpy ~0.05ms` `SIMD`, so that universal DAW support is proven.

## Implementation Decisions
- Debt PR `07` is one PR: `add_library(wha_common INTERFACE)` with `target_include_directories` + `target_compile_features(cxx_std_20)`, `constexpr` for all `WHA_*`/`WHAA_*`, `std::atomic<int32_t> clientCount`/`ready`/`activeBuf`/`mixedActive` with `fetch_add`/`load`/`store`, single `IsValidNetworkChannels` in `WHASlotTable.h` called by `WHAPacket.h`/`WHASlotsJson.h`, central `static_assert`s, `SetSlotName` `TruncateCopy` helper, `tanh` stays in header for now (Worker moves it later).
- `ASIO SDK 2.3.3` vendored under `third_party/asio/` per ADR 0007, `.gitignore` payload, stub `IASIO` for offline build, real SDK for runtime.
- `08` minimal `IASIO`: `DllGetClassObject`/`DllRegisterServer`/`DllUnregisterServer`, `class WinHookMasterASIO : public IASIO`, `init` creates `Global\WinHookAudio_SlotTable` `80KB` + `Global\WinHookAudio_Master_Audio` `16MB` + `4× Bridge Shared` `8MB` + events, `getChannels`/`getChannelInfo`/`getSampleRate`/`getBufferSize`/`createBuffers`/`start`/`stop`/`bufferSwitch` (`memcpy` only) + `controlPanel` stub `ASE_OK`, no Worker, no KS, `stream_verified:false`.
- `09` Worker: `MasterHolder.cpp` `MMCSS Pro Audio` thread `WaitForSingleObject(Master_Tick)`, per-thing FIFOs `HW 64`/`Virtual 256`/`Bridge 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms`, Loopback `if (masterOut[i].loopback && type==VIRTUAL) memcpy masterIn[paired]`, Bridge sum `sum+=clientIn[clientId][active][ch]` `mixed=tanh(sum)` `3ms` timeout per Bridge, `SetEvent(Bridge_Tick[clientId])` broadcast, still no KS/WHAA I/O.
- `10` `KS` exclusive: `WASAPI Exclusive`/`KS` `hwBuffer 64` to Real HW, `DeviceIoControl` not yet (Virtual Cable deferred), `UDP 6980/6981` not yet (WHAA deferred), audible tone `DAW → SHM Out → Worker → KS → HW`, measured latency `1000×frames/sampleRate` + `memcpy` time, `stream_verified:true` only after measurement.
- `Bridge DLL` `08` minimal: `WinHookAudioBridgeASIO64.dll` one file `4 CLSIDs` `B1..B4` → `g_instance 0..3` → `clientId 0..3` via `InterlockedIncrement`/`fetch_add`, `getChannels` counts `masterIn where type==BRIDGE1+g_instance`, `getChannelInfo` iterates to nth, `bufferSwitch` writes `clientIn[clientId][active]` and waits on `Bridge_Tick[clientId]`, `5th client → ASE_NotPresent` `Bridge1 full (4/4)`.
- C++20 user-mode, `MSVC` `x64` `MultiThreaded`, `/W4 /WX /permissive- /utf-8 /EHsc`, `UNICODE` `WIN32_LEAN_AND_MEAN`, `ole32` `propsys` `setupapi` `ksuser` `advapi32` `uuid`, `ImGui`/`libvorbis` not in this spec (Panel/Network deferred).
- Vocabulary: `Master DAW`/`Slave DAW`/`Master Driver`/`Bridge Driver`/`Virtual Cable`/`Slot`/`Slot Table`/`Master Clock`/`Loopback`/`Shared Bridge`/`Network Stream`/`Control Panel`/`Worker` per `CONTEXT.md` + ADR 0007.

## Testing Decisions
- Good tests check external behavior only: `wha_common` include path, `constexpr` values, `atomic` 5th client rejection, `32/128` single source, SHM size asserts, `getChannels` `1..512`, `"- empty -"` mapping, `ASIOResetRequest`, `bufferSwitch` `memcpy` + `Master_Tick`, Worker `Loopback` next tick, Bridge `tanh` sum + timeout, `KS` exclusive open, SDK host sample `CoCreateInstance` flow, Bitwig 512 names visible.
- Modules tested: `wha_common` ABI, `asio-master` `IASIO`, `asio-bridge` `IASIO`, Worker, `KS` adapter.
- Prior art: `tests/Abi-Check.ps1` + `tests/Probe-Cli.ps1` offline pattern; extend with `host/sample` offline `IASIO` test and `ctest` `stream_verified` flag.
- All offline tests run without admin, DAW, device, network, `stream_verified:false`. Runtime tests run in elevated new `cmd` with Bitwig + Real HW, `stream_verified:true` only after measured tone.
- Failure cases: `masterInCount`/`masterOutCount` `0`/`513`, overlong `name[32]`, `loopback` on non-`VIRTUAL`, `5th Bridge client` `ASE_NotPresent`, `WHAA` invalid `codec`/`channels`/`streamId`/`payload`, `KS` `ASE_NoMemory`/`ASE_NotPresent` on busy pin.

## Out of Scope
- Virtual Cable `WinHookAudio.sys` `8× Stereo` `IOCTL_WHA_READ/WRITE`/`SET_LOOPBACK` and `64KB RingBuffer`.
- WHAA Network `UDP 6980/6981` `PCM_F32`/`PCM_I16`/`VORBIS Q0.1–1.0` + `CODEBOOK` handshake + `jitter` + `r8brain SRC` + `discovery`.
- Control Panel Popup Type 1 `controlPanel() → CreateThread → ImGui DX11` `INPUTS 512 | OUTPUTS 512 | NETWORK 8 | GENERAL | ABOUT`.
- Installer `5 CLSIDs` + `WinHookAudio.reg` + `Inno Setup` + firewall `6980-6981` + `EV Cert`/`Attestation`.
- `r8brain-free`/`moodycamel::ReaderWriterQueue`/`libvorbis` integration beyond Worker FIFO sizing.

## Further Notes
- Respects ADRs 0001–0007: `512` pool, Pure DLL Worker, `WHAA` only, Shared sum, Opt-in Loopback, Five-tab panel, `ASIO SDK` vendoring.
- `v10.2` governs over `v10.1` where they conflict; `v10.1` ABI slice remains green as offline gate.
- No audio claim without runtime verification (`stream_verified:true` only after Bitwig + `KS` measured).
