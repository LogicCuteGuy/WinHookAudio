# WHAA Vorbis + r8brain vendoring

Date: 2026-09-16
Status: accepted

## Context
WHAA Network needs `PCM_F32`/`PCM_I16`/`VORBIS Q0.1–1.0` + `CODEBOOK` handshake + `jitter 20/50ms` + `r8brain SRC` if remote `SR` differs, `UDP 6980` audio + `6981` discovery `WHAA_HELLO 2s`, `NETWORK 8` tab `Tx1..8`/`Rx1..8` `192.168.1.50:6980` `PCM_F32`/`VORBIS` `Q0.4` `Ch32` `BW`. `WHAPacket.h` + `WHASlotTable` `netTx[8]`/`netRx[8]` + `WHAA CODEBOOK` already exist offline, `Worker` `MasterHolder` already has `Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` `Per-Thing` `FIFOs` but no `UDP` `sendto`/`recvfrom`/`jitter`/`decode`/`SRC`. `ADR 0007` (`ASIO SDK 2.3.3` under `third_party/asio/`) + `ADR 0008` (`ImGui 1.90` under `third_party/imgui/`) both use `third_party/` + `.gitignore` payload + `README` fetch step + stub for offline build.

## Decision
Vendor `libvorbis 1.3.7 + libogg 1.3.5 + r8brain-free` under `third_party/libvorbis/` + `third_party/r8brain/` like `ASIO`/`ImGui`, `.gitignore` payload, `README` fetch step, stub `Vorbis` encode/decode as `PCM` fallback for offline `ctest` (`stream_verified:false`). First slice is `PCM_F32`/`PCM_I16` + `jitter 20ms` + `CODEBOOK 3×` on `Save` + `NETWORK 8` tab without `Vorbis` encode, `Vorbis Q0.1–1.0` + `libvorbis` + `r8brain` follow as `15` with `third_party/libvorbis/` like `ASIO SDK`/`ImGui`. `MasterHolder::doTick()` does `Tx: SHM Out → PCM payload → sendto 6980` + `Rx: recvfrom nonblock → jitter 20ms → pop on Master_Tick → SHM In`, one `MMCSS Pro Audio` thread, `jitter` as `std::queue` per `streamId 0..7`. `6981 WHAA_HELLO 2s` + `netsh 6980-6981` deferred to second slice to keep offline gate without `ADMIN`.

## Consequences
- `PCM` `UDP 6980` `sendto`/`recvfrom` + `jitter 20ms` + `CODEBOOK 3×` proven without `libvorbis`/`r8brain` vendoring.
- `Vorbis Q0.1–1.0` `64–500 kbps` per stereo + `libvorbis` + `r8brain` follow as `15` with `third_party/libvorbis/` like `ASIO SDK`/`ImGui`.
- `NETWORK 8` tab `Tx1..8`/`Rx1..8` `192.168.1.50:6980` `PCM_F32`/`VORBIS` `Q0.4` `Ch32` `BW` in first slice, `WHAControlPanelUI.cpp` one `ImGui` code.
- `third_party/libvorbis/` + `third_party/r8brain/` not redistributed publicly; CI must fetch via documented step.
- `Worker` `MasterHolder` one `MMCSS Pro Audio` thread keeps `Master Clock 2.7ms@128/48k` `ONE tick for all 512ch`, `jitter 20ms` is `~7` ticks `128/48k`.
