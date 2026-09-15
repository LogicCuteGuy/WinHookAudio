# 10: Master KS exclusive — audible tone

**What to build:** `KS` exclusive `hwBuffer 64` to Real HW (Scarlett/Realtek) via `WASAPI Exclusive`/`KS`, audible tone `DAW → SHM Out → Worker → KS → HW`, measured latency, `stream_verified:true` only after measurement.

**Blocked by:** 09-master-worker.

**Status:** ready-for-agent

- [ ] `KS` exclusive `hwBuffer 64` to Real HW, `WASAPI Exclusive` `IAudioClient::Initialize(EXCLUSIVE)` + `GetService(IAudioRenderClient)` or `KsCreatePin` `KSPIN_CONNECT` + `KSDATAFORMAT_WAVEFORMATEXTENSIBLE`, `DeviceIoControl` not yet (Virtual Cable deferred), `UDP 6980/6981` not yet (WHAA deferred)
- [ ] Audible tone `DAW → SHM Out → Worker → KS → HW`, `512ch ×128×4×2=16MB` `memcpy ~0.05ms` `SIMD` measured, `1000×frames/sampleRate` `2.7ms@128/48k` Master Clock, `HW 64` `1.33ms` low-latency path
- [ ] SDK `host/sample` then Bitwig runtime gate in elevated new `cmd` (`ADMIN=True`), Bitwig `Preferences → Audio → ASIO → WinHookAudio Master` shows `512` names, `stream_verified:true` only after measured tone, `probe` `stream_verified:false` stays for enumeration-only
- [ ] Failure cases: `KS` `ASE_NoMemory`/`ASE_NotPresent` on busy pin, `masterInCount`/`masterOutCount` `0`/`513`, `5th Bridge client` `ASE_NotPresent`, `WHAA` invalid `codec`/`channels`/`streamId`/`payload` still rejected offline
