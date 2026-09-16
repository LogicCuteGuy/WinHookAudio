---
name: winhookaudio
description: Windows ASIO driver project — C++20 user-mode DLLs (Master/Bridge), KS/WASAPI adapters, virtual cable kernel driver, and native Win32 control panel hosted by the DAW. Use for any WinHookAudio task.
---

# WinHookAudio

Production C/C++ ASIO driver where the DAW is the host. The driver exposes KS devices, virtual cables, ASIO bridges, and LAN audio through a native C++ control panel. See `docs/architecture.md`, `docs/implementation-plan.md`, and `CONTEXT.md`.

## When to use

- Any task in this repo: ASIO DLLs (`asio-master/`, `asio-bridge/`), probe/ABI checks (`src/`), common ABI (`common/`), tests, CMake/MSVC build, or docs/prototype.
- Implementing stages from `docs/implementation-plan.md` (probe → primary driver → clock adaptation → cable/bridge → LAN → panel/packaging).
- Reviewing or writing C++20 code — also apply `cpp-coding-standards` (C++ Core Guidelines).

## Terminology (from CONTEXT.md)

Use exact terms: **Master DAW** (not Main/host DAW), **Bridge Driver** (not Slave/client driver), **Slot/Slot Table** (not Channel), **Master Clock**, **Loopback**, **Shared Bridge**, **Network Stream**, **Control Panel**, **Worker**. Everything in the grid is named from the Master DAW's perspective.

## Architecture constraints

- **Host model:** DAW loads the ASIO DLL (`WinHookAudioMasterASIO64.dll` / `WinHookAudioBridgeASIO64.dll`). No standalone audio engine replaces the DAW.
- **Languages:** C++20 for user-mode driver/runtime/panel; C-compatible ABI at boundaries; WDK C/C++ for the separate virtual cable kernel driver. HTML prototype is layout reference only.
- **Channel bank:** 32-in/32-out proposed default; unassigned inputs = silence, unused outputs = discarded; disconnects never renumber DAW channels.
- **Clock:** One Master Clock; every independent device crosses via bounded FIFO + adaptive resampling. Equal nominal rates ≠ synchronized.
- **No claims without runtime verification:** Do not claim audio functionality without measured evidence (see `docs/architecture.md`).

## Project structure

- `asio-master/` — Master driver DLL (IASIO stub, MasterHolder, KsAudio)
- `asio-bridge/` — Bridge driver DLL
- `common/` — Shared ABI (`wha_common` interface, `WHABridgeShared.h`, `ASIOStub.h`)
- `src/probe/` — `winhookaudio-probe` (MMDevice/KS/ASIO registry enumeration)
- `src/abi-check/` — ABI stability check
- `tests/` — `host-sample`, `worker-test`, `panel-test`, `ks-test` + `Probe-Cli.ps1`/`Abi-Check.ps1`/`Host-Sample.ps1`
- `build/` — CMake/MSVC x64 out-of-tree build (already configured)
- `docs/`, `prototype/index.html`, `scripts/Get-AudioInventory.ps1`

## Build & test (Windows/MSVC/x64 only)

Requires Windows + MSVC + x64 (`CMakeLists.txt` enforces this). C++20, `/W4 /WX /permissive- /utf-8 /EHsc`, `MultiThreaded$<$<CONFIG:Debug>:Debug>`.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
# or single probe:
.\build\Release\winhookaudio-probe.exe
powershell -ExecutionPolicy Bypass -File tests/Probe-Cli.ps1 -Probe .\build\Release\winhookaudio-probe.exe
```

Targets: `winhookaudio-probe`, `winhookaudio-abi-check`, `winhookaudio-master`, `winhookaudio-bridge`, `winhookaudio-host-sample`, `winhookaudio-worker-test`.

## Instructions

1. Read `docs/architecture.md` and `docs/implementation-plan.md` before changing driver/transport code; respect stage acceptance criteria.
2. Keep ABI stable: versioned shared-memory with fixed-width fields; never share raw pointers or compiler-dependent structs across DLL boundaries.
3. Audio thread rules: no file I/O, heap alloc, GUI calls, unbounded waits, or blocking mutex in the callback/worker hot path. Use preallocated SPSC rings, monotonic seqnos, generation IDs.
4. Validate formats/periods against actual device caps; separate DAW rate, ASIO sample type, and endpoint format.
5. Test on-device (VS 2022/18 Community, CMake via VS/vswhere) and report measured latency/underruns, not just build success.
6. Follow `cpp-coding-standards` (RAII, const-by-default, strong types, value semantics) when writing C++.
