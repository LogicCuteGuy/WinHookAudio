# Virtual Cable WDK PortCls WaveRT

Date: 2026-09-16
Status: accepted

## Context
Virtual Cable needs `WinHookAudio.sys` `8× Stereo` `16ch` `Virtual 1..8 L/R` `64KB RingBuffer` `DeviceIoControl IOCTL_WHA_READ/WRITE/SET_LOOPBACK` `WDK` `PortCls` `WaveRT` `MSVAD` `Root\WinHookAudio` `WinHookAudio.inf/.cat`, `MasterHolder` `DeviceIoControl` wiring, `GENERAL` `Virtual Cables: [8 v] 8× Stereo Name[WinHookAudio Virtual]` + `Alternative mode 1× 64ch`. `SLOT_VIRTUAL` `2` + `virtualBuffer 256` + `IsLoopbackValid` + `PanelFilter::Virtual` + `SetInputLoopback` `VIRTUAL` only already exist, `MasterHolder` loopback stub `memcpy` `OUT→IN` already proves `VRChat → Virtual1 → DAW → Virtual3 LOOP → VRCT` `~5.4ms` without `sys`. `ADR 0007` (`ASIO SDK 2.3.3` under `third_party/asio/`) + `ADR 0008` (`ImGui 1.90` under `third_party/imgui/`) + `ADR 0009` (`libvorbis`/`r8brain` under `third_party/libvorbis/`/`r8brain/`) all use `third_party/` + `.gitignore` payload + `README` fetch step + stub for offline build.

## Decision
Vendor `WDK 11 22621` `PortCls` `WaveRT` `MSVAD` sample under `third_party/wdk/` like `ASIO`/`ImGui`/`libvorbis`, `.gitignore` payload, `README` fetch step, stub `WinHookAudio.sys` as `memcpy` fallback for offline `ctest` (`stream_verified:false`). First slice is `8× Stereo` `16ch` `Virtual 1..8 L/R` `64KB` `RingBuffer` `IOCTL_WHA_READ/WRITE/SET_LOOPBACK` + `MasterHolder::doTick()` `Virtual: DeviceIoControl(IOCTL_WHA_READ) → SHM In` + `SHM Out → DeviceIoControl(IOCTL_WHA_WRITE)` + `Loopback: if Master OUT loopback=v → copy to Master IN next tick` + `SET_LOOPBACK` for `Virtual3` `Ring` → `VRCT`, one `MMCSS Pro Audio` thread, `RingBuffer` as `std::array<float,64*1024>` per `Virtual` `1..8`, `GENERAL` `Virtual Cables: [8 v] 8× Stereo Name[WinHookAudio Virtual]` in first slice. `WinHookAudio.sys` `WDK` `PortCls` `WaveRT` + `WinHookAudio.inf/.cat` + `testsigning` + `ADMIN=True` follow as `18` with `WDK` `22621` + `Bitwig` runtime gate.

## Consequences
- `8× Stereo` `16ch` `Virtual 1..8 L/R` `64KB` `RingBuffer` + `IOCTL_WHA_READ/WRITE/SET_LOOPBACK` + `MasterHolder` `DeviceIoControl` wiring proven without `WDK` `PortCls` `WaveRT` in first slice stub.
- `Loopback` `~5.4ms` `2×128` without extra `DAW` track proven via `MasterHolder` `memcpy` `OUT→IN` + `SET_LOOPBACK` for `Virtual3` `Ring`.
- `GENERAL` `Virtual Cables: [8 v] 8× Stereo Name[WinHookAudio Virtual]` + `Alternative mode 1× 64ch` in first slice, `WHAControlPanelUI.cpp` one `ImGui` code.
- `third_party/wdk/` not redistributed publicly; CI must fetch via documented step.
- `Worker` `MasterHolder` one `MMCSS Pro Audio` thread keeps `Master Clock 2.7ms@128/48k` `ONE tick for all 512ch`, `Virtual 256` `Per-Thing` `FIFO` is `~2` ticks `128/48k`.
