# 15: WHAA Vorbis + r8brain SRC

**What to build:** `VORBIS Q0.1–1.0` `64–500 kbps` per stereo `Per-stream` `codec` `Q` + `libvorbis 1.3.7 + libogg 1.3.5 + r8brain-free` vendored under `third_party/libvorbis/` + `third_party/r8brain/` like `ASIO`/`ImGui`, `jitter Vorbis 50ms` `Network Vorbis 1024`.

**Blocked by:** 14-pcm-jitter-codebook.

**Status:** ready-for-agent

- [ ] `VORBIS Q0.1–1.0` `64–500 kbps` per stereo `Per-stream` `codec` `Q` + `libvorbis 1.3.7 + libogg 1.3.5 + r8brain-free` vendored under `third_party/libvorbis/` + `third_party/r8brain/` like `ASIO`/`ImGui`, `.gitignore` payload, `README` fetch step, stub `Vorbis` as `PCM` fallback for offline `ctest` replaced with real `libvorbis` encode/decode + `r8brain` `SRC` if remote `SR` differs, `jitter Vorbis 50ms` `Network Vorbis 1024`, `Vorbis` `30–50 ms` `WAN`
