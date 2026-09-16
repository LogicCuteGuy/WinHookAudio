# 14: WHAA PCM + jitter + CODEBOOK

**What to build:** `WHAA` `PCM_F32`/`PCM_I16` `UDP 6980` `sendto`/`recvfrom` + `jitter PCM 20ms` `Per-Thing` `FIFOs` `Network PCM 512` + `CODEBOOK` handshake `3×` `redundant` on `Save` without `Vorbis` encode.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] `WHAA` `PCM_F32`/`PCM_I16` `UDP 6980` `sendto`/`recvfrom` + `jitter PCM 20ms` `Per-Thing` `FIFOs` `Network PCM 512` + `CODEBOOK` handshake `3×` `redundant` on `Save` without `Vorbis` encode, `MasterHolder::doTick()` `Tx: SHM Out → PCM payload → sendto 6980` + `Rx: recvfrom nonblock → jitter 20ms → pop on Master_Tick → SHM In`, one `MMCSS Pro Audio` thread, `jitter` as `std::queue` per `streamId 0..7`, `UDP` `127.0.0.1:6980` loopback without `firewall`/`ADMIN`, `stream_verified:false` offline `ctest` green
