# 13: Panel ABOUT + Save contract

**What to build:** `ABOUT` tab + full `Save` contract (`memcpy SHM` + `version++` + `slots.json` + `FlushViewOfFile` + `SetEvent(TableChanged)` + `hostCallback(ASIOResetRequest)`).

**Blocked by:** 12-panel-outputs-general.

**Status:** ready-for-agent

- [ ] `ABOUT` `Version`, `WinHookAudio.sys` running, `5 CLSIDs`, `%ProgramData%\WinHookAudio\slots.json`, `Export/Import`, `Firewall UDP 6980-6981 [Fix]`, `Reset Default`, `Bridge clients: Bridge1 2/4`
- [ ] `Save` full contract: `editCopy → *pTable, version++, WriteFile %ProgramData%\WinHookAudio\slots.json, FlushViewOfFile, SetEvent(TableChanged), hostCallback(ASIOResetRequest) → DAW re-calls getChannels/getChannelInfo → "- empty -" grey, loopback active, new bridge clients re-count`, `Per-Thing` `FIFO` change reloads `Worker` without `host reset`
- [ ] `Bridge` popup filtered to `BRIDGE1` only, `GENERAL` read-only `Follows Master 48000/32/128`, offline `ctest` green without admin/DAW/device/network, `stream_verified:false`, `slots.json` round-trip, `version++`, `TableChanged`, `ASIOResetRequest` verified via `host-sample` stub
