# 12: Panel OUTPUTS 512 + GENERAL Per-Thing

**What to build:** `OUTPUTS 512` tab (independent indices, `Out03` does not move `In03`) + `GENERAL` `Master Clock` `48000`/`32 Float`/`128` `2.7ms` + `Per-Thing` `HW 64`/`Virtual 256`/`Bridge1..4 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms`.

**Blocked by:** 11-panel-inputs.

**Status:** ready-for-agent

- [ ] `OUTPUTS 512` (`masterOut[512]` only, independent indices) `ListClipper` `Filter`/`Search` `Loop` (`VIRTUAL` only) `En` `X`, moving `Out03` does not move `In03`, `Loop` pairs `Out03`→`In03` next tick via `Worker`
- [ ] `GENERAL` `Master Clock` `48000`/`32 Float`/`128` `2.7ms` `ONE tick for all 512ch` with `Save & Reset DAW → ASIOResetRequest`, `Per-Thing` `HW 64`/`Virtual 256`/`Bridge1..4 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms` without `host reset` (Worker reloads via `TableChanged`), `MMCSS`/`Exclusive`/`Virtual Cables 8`/`Ports 6980/6981`/`Perf`
- [ ] Offline `ctest` green without admin/DAW/device/network, `stream_verified:false`
