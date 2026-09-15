# 09: Master Worker — MMCSS + Loopback + Shared sum

**What to build:** Worker `MMCSS Pro Audio` thread `WaitForSingleObject(Master_Tick)` with per-thing FIFOs, Loopback `OUT→IN` next tick, Shared Bridge `tanh` sum + `3ms` timeout, `4× Bridge ×4 clients` broadcast.

**Blocked by:** 08-master-minimal.

**Status:** ready-for-agent

- [ ] `MasterHolder.cpp` Worker `MMCSS Pro Audio` thread `WaitForSingleObject(Master_Tick)`, per-thing FIFOs `HW 64`/`Virtual 256`/`Bridge 128`/`Network PCM 512`/`Vorbis 1024` + `Jitter 20/50ms`, `for(i<masterInCount)` `if(NONE) silence else if(HW) KS else if(VIRTUAL) IOCTL else if(NETWORK) jitter.pop else if(BRIDGE) sum`
- [ ] Loopback `if (masterOut[i].loopback && type==VIRTUAL) memcpy masterIn[paired]` next tick, `~5.4ms` `2×128` loop `VRChat → Virtual1 → DAW → Virtual3 LOOP → VRCT` without extra track, isolated by default
- [ ] Shared Bridge sum `sum+=clientIn[clientId][active][ch]` `mixed=tanh(sum)` soft-clip `2 DAWs at -6dB → 0dB`, `3ms` timeout per Bridge `ready[0..3]`, lagging client silenced that tick, `SetEvent(Bridge_Tick[clientId])` broadcast to `4` clients, `mixedIn[2][64][4096]` + `mixedActive`
- [ ] Offline checks pass with no admin, DAW, device, network, `stream_verified:false`, `ctest` green, no `KS`/`WHAA` I/O yet
