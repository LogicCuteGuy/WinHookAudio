# 17: Virtual Ring + IOCTL

**What to build:** `WinHookAudio.sys` `8× Stereo` `16ch` `Virtual 1..8 L/R` `64KB RingBuffer` `DeviceIoControl IOCTL_WHA_READ/WRITE/SET_LOOPBACK` `WDK` `PortCls` `WaveRT` `MSVAD` `Root\WinHookAudio` `WinHookAudio.inf/.cat`, `MasterHolder::doTick()` `Virtual: DeviceIoControl(IOCTL_WHA_READ) → SHM In` + `SHM Out → DeviceIoControl(IOCTL_WHA_WRITE)` + `Loopback: if Master OUT loopback=v → copy to Master IN next tick` + `SET_LOOPBACK` for `Virtual3` `Ring` → `VRCT`.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] `WinHookAudio.sys` `8× Stereo` `16ch` `Virtual 1..8 L/R` `64KB RingBuffer` `DeviceIoControl IOCTL_WHA_READ/WRITE/SET_LOOPBACK` `WDK` `PortCls` `WaveRT` `MSVAD` `Root\WinHookAudio` `WinHookAudio.inf/.cat`, `MasterHolder::doTick()` `Virtual: DeviceIoControl(IOCTL_WHA_READ) → SHM In` + `SHM Out → DeviceIoControl(IOCTL_WHA_WRITE)` + `Loopback: if Master OUT loopback=v → copy to Master IN next tick` + `SET_LOOPBACK` for `Virtual3` `Ring` → `VRCT`, one `MMCSS Pro Audio` thread, `RingBuffer` as `std::array<float,64*1024>` per `Virtual` `1..8`, `WDK 11 22621` `PortCls` `WaveRT` `MSVAD` sample vendored under `third_party/wdk/` like `ASIO`/`ImGui`/`libvorbis`, `.gitignore` payload, `README` fetch step, stub `WinHookAudio.sys` as `memcpy` fallback for offline `ctest` (`stream_verified:false`)
