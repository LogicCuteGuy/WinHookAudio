# 08: Master minimal IASIO — DAW hosts DLL

**What to build:** Minimal `IASIO` proving Bitwig hosts `WinHookAudio Master` with `512 In + 512 Out` dynamic, stable indices, `"- empty -"` silence, `hostCallback(ASIOResetRequest)` on `+Add Input`, `bufferSwitch` `memcpy` only.

**Blocked by:** 07-debt-pr.

**Status:** ready-for-agent

- [ ] `ASIO SDK 2.3.3` vendored under `third_party/asio/` per ADR 0007, `.gitignore` payload, stub `IASIO` for offline build, `DllGetClassObject`/`DllRegisterServer`/`DllUnregisterServer`, `class WinHookMasterASIO : public IASIO`
- [ ] `init` creates `Global\WinHookAudio_SlotTable` `80KB` + `Global\WinHookAudio_Master_Audio` `16MB` + `4× Bridge Shared` `8MB` + `Master_Tick`/`TableChanged`/`Bridge_Tick[4][4]` events, `getChannels(*in=masterInCount,*out=masterOutCount)` `1..512`, `getChannelInfo` `"- empty -"` for `SLOT_NONE` else `name[32]`, `getSampleRate`/`getBufferSize` from `WHAGeneral`
- [ ] `createBuffers`/`start`/`stop`/`bufferSwitch` does only `DAW In ← SHM In` + `SHM Out ← DAW Out` + `SetEvent(Master_Tick)` `memcpy`, no Worker, no KS, `controlPanel` stub `ASE_OK`, `stream_verified:false`
- [ ] `Bridge DLL` minimal: `WinHookAudioBridgeASIO64.dll` one file `4 CLSIDs` `B1..B4` → `g_instance 0..3` → `clientId 0..3` via `fetch_add`, `getChannels` counts `BRIDGE1+g_instance`, `getChannelInfo` iterates to nth, `bufferSwitch` writes `clientIn[clientId][active]` and waits on `Bridge_Tick[clientId]`, `5th client → ASE_NotPresent`
- [ ] SDK `host/sample` `CoCreateInstance(CLSID_Master)` → `init` → `getChannels` → `getChannelInfo` → `createBuffers` → `start` → `bufferSwitch` loopback without DAW, offline `ctest` green without admin
