# WinHookAudio Datasheet — v10.1 Data Architecture & Use Case Plans

**Brand:** WinHookAudio **| Concept:** DAW = Man in the Middle — Master DAW MUST be open (by design)  
**Pool:** **512 In + 512 Out absolute** `masterIn[512] + masterOut[512]` separate indices | **Architecture:** Pure DLL `controlPanel()` Type 1 | **Version:** v10.1 — 2026-09-14

> This is the **expanded Data Architecture** companion to **v10 Datasheet** (which covers features, file structure, and GUI). Use this for implementation and for planning your DAW template before you build.

* * *

## 1. Data Architecture Overview — Control Plane vs Data Plane

```
                        ┌─────────────────────────────────┐
                        │        CONTROL PLANE (80KB)      │
                        │  WHASlotTable v10 (versioned)    │  User edits in GUI Popup Thread
                        │  INPUTS 512 + OUTPUTS 512        │  File: %ProgramData%\WinHookAudio\slots.json
                        │  GENERAL Per-Thing Buffers       │  SHM: Global\WinHookAudio_SlotTable
                        │  NETWORK 8 streams               │  Event: TableChanged → Worker reload (no audio copy)
                        └──────────────┬──────────────────┘
                                       │ hostCallback(ASIOResetRequest) when count/name changes
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              DATA PLANE (48 MB SHM)                                │
│  Master_Audio 16MB  float[2][512][4096]  ping-pong  Worker ↔ Master DAW            │
│  Bridge1_Shared 8MB  clientIn[4][2][64][4096] + clientOut[4][2][64][4096] + mixed   │
│  Bridge2/3/4_Shared 8MB ×3                                                          │
│  Virtual Rings 8×64KB in WinHookAudio.sys (kernel)  IOCTL_WHA_READ/WRITE            │
│  Network Ring UDP 6980 PCM/Vorbis  Jitter PCM 20ms / Vorbis 50ms  WHAA              │
│  ── Master_Tick Event every bufferSwitch(128) = 2.7ms @48k — drives ALL data ──    │
│  Data never touches Control Plane except ready[4] sum and loopback copy           │
└─────────────────────────────────────────────────────────────────────────────────────┘

Thread separation (never block DAW):
- DAW bufferSwitch thread (real-time): ONLY memcpy SHM ↔ DAW buffers
- Worker MMCSS Pro Audio (inside Master DLL): Handles WASAPI KS / IOCTL / UDP / Bridge sum / Loopback / SRC
- Network Rx/Tx threads: recvfrom / encode Vorbis (libvorbis) outside Master_Tick
- UI Thread: ImGui DX11, edits copy, Save triggers ASIOResetRequest
```

* * *

## 2. SHM Memory Map — Exact Layout

| Region | SHM Name | Size | Created by | Contents & Access |
| --- | --- | --- | --- | --- |
| SlotTable | `Global\WinHookAudio_SlotTable` | 80 KB | Master `init()` | `WHASlotTable v10` + `version` + `WHAGeneral` + `netTx/Rx[8]` — `CreateFileMapping` `88,064 B` |
| Master Audio | `Global\WinHookAudio_Master_Audio` | 16 MB | Master | `struct { volatile LONG active; float buf[2][512][4096]; }` = `2×512×4096×4 = 16,777,216 B` ping-pong |
| Bridge1 Shared | `Global\WinHookAudio_Bridge1_Shared` | 8 MB | Master | `WHABridgeShared` see §7 — 4 clients |
| Bridge2 Shared | `Global\WinHookAudio_Bridge2_Shared` | 8 MB | Master | same |
| Bridge3 Shared | `Global\WinHookAudio_Bridge3_Shared` | 8 MB | Master | same |
| Bridge4 Shared | `Global\WinHookAudio_Bridge4_Shared` | 8 MB | Master | same |
| Events | `Global\WinHookAudio_Master_Tick` | Event | Master | `SetEvent` by Master `bufferSwitch()` → Worker `WaitForSingleObject(INFINITE)` |
|  | `Global\WinHookAudio_Bridge1_Tick[0..3]` ×4 bridges ×4 clients | Event ×16 | Worker | Worker broadcasts to each Bridge client after summing |
|  | `Global\WinHookAudio_TableChanged` | Event | Master GUI | `SetEvent` on Save → Worker reloads `version` |
| File | `%ProgramData%\WinHookAudio\slots.json` | JSON | GUI Save | Persistent copy of SlotTable + general, loaded on next Master `init()` |

**WHABridgeShared per Bridge (8 MB):**

```cpp
struct WHABridgeShared {
  volatile LONG clientCount;       // 0..4 InterlockedIncrement
  volatile LONG ready[4];          // 1 = new data this tick per client
  volatile LONG activeBuf[4];      // 0/1 ping-pong per client
  float clientIn[4][2][64][4096];  // Slave DAW OUT → Master (per client, per buffer)
  float clientOut[4][2][64][4096]; // Master OUT → Slave DAW (broadcast to all 4)
  float mixedIn[2][64][4096];      // Worker sum of 4 clients → Master In subset
  volatile LONG mixedActive;
};
// 4×2×64×4096×4 = 8,388,608 B per bridge
```

All 48 MB (`80KB + 16MB + 32MB`) pre-allocated on first Master `init()`; Bridge only `OpenFileMapping` — closing Master DAW → `UnmapViewOfFile` → silence by design.

* * *

## 3. WHASlotTable v10 Schema — Data Definition

```cpp
#pragma pack(push,1)
#define WHA_MAX 512
#define WHA_BRIDGE_COUNT 4
#define WHA_NAME_LEN 32
#define WHA_NET_STREAMS 8

enum WHASlotType : uint32_t {
  SLOT_NONE=0,        // "- empty -" silence, keeps DAW index for templates
  SLOT_HW=1,          // Real Mic/Speaker via WASAPI Exclusive KS
  SLOT_VIRTUAL=2,     // WinHookAudio.sys 8× Stereo Virtual 1..8 L/R
  SLOT_NETWORK=3,     // WHAA PCM/Vorbis stream
  SLOT_BRIDGE1=4, SLOT_BRIDGE2=5, SLOT_BRIDGE3=6, SLOT_BRIDGE4=7
};

struct WHASlot {
  WHASlotType type;
  int32_t  srcChannel; // 0..N inside type (HW Ch, Virtual cable L/R, Network ch, Bridge ch)
  int32_t  streamId;   // NETWORK only 0..7
  char     name[WHA_NAME_LEN]; // "SM58 Mic" "Discord" "FL+Live Sum" "Loopback VRCT LOOP" → getChannelInfo()
  bool     enabled;    // false = silence but channel still exists (counts in getChannels)
  bool     loopback;   // v10: only valid if type==VIRTUAL — Worker copies OUT→IN next tick
  uint8_t  _pad[2];
};

struct WHAGeneral {
  // MASTER CLOCK — what DAW sees via IASIO::getSampleRate / getBufferSize — ONE for all 512
  uint32_t sampleRate = 48000; // 44100 / 48000 / 96000
  uint32_t bitDepth = 32;      // ASIOSTFloat32LSB fixed (internal float, network PCM_I16 is conversion)
  uint32_t asioBuffer = 128;   // 64/128/256/512/1024 — 2.7ms@128/48k — drives ALL data plane
  // PER-THING WORKER FIFOs — adapts to Master Clock (in GENERAL Tab, NOT per slot) — no DAW reset on change
  uint32_t hwBuffer = 64;                // HW KS low latency
  uint32_t virtualBuffer = 256;          // Virtual stable
  uint32_t bridgeBuffer[4] = {128,128,128,128}; // per Bridge instance
  uint32_t networkPcmBuffer = 512;       // PCM FIFO
  uint32_t networkVorbisBuffer = 1024;   // Vorbis FIFO (needs 256+ frames for encode)
  uint32_t jitterPcm = 20;   // ms
  uint32_t jitterVorbis = 50;// ms
};

struct WHANetworkStream {
  char ip[16]; // "192.168.1.50" 0.0.0.0 = disabled
  uint16_t port = 6980;
  enum WHACodec { PCM_F32, PCM_I16, VORBIS } codec = PCM_F32;
  float quality = 0.4f; // Vorbis 0.1(64k) .. 0.4(192k) .. 0.6(256k) .. 1.0(500k) per stereo
  uint32_t channels = 2; // 1..32 PCM safe MTU, 1..128 Vorbis compressed
};

struct WHASlotTable {
  uint32_t version;           // ++ on Save, Worker checks TableChanged event
  uint32_t masterInCount;     // 1..512 dynamic — INPUTS Tab — getChannels().in
  WHASlot  masterIn[512];     // Input index 0 = DAW Input 1 (independent from masterOut)
  uint32_t masterOutCount;    // 1..512 dynamic — OUTPUTS Tab — getChannels().out
  WHASlot  masterOut[512];    // Output index 0 = DAW Output 1
  WHAGeneral general;
  WHANetworkStream netTx[8], netRx[8]; // My Version Only — maps to slots where Type==NETWORK
};
#pragma pack(pop)
```

**Invariants:** `BRIDGE1..4` are Types inside `512`, not extra pool. `SLOT_NONE` still counts in `masterInCount`/`masterOutCount` — DAW sees `"- empty -"` grey, Worker fills `0.0f` — preserves template indices. `loopback` only meaningful on `VIRTUAL` slots.

* * *

## 4. Audio Pipeline & Buffer Adaptation — Per-Thing GENERAL

Master DAW `bufferSwitch(128)` = **ONE Master Clock 2.7ms** for all 512ch. Worker adapts per-thing FIFOs:

```
Master Clock tick (128 @48k = 2.7ms)
  ├─ bufferSwitch thread (real-time, DO NOT BLOCK):
  │   DAW In[0..masterInCount)  ← memcpy ← Master_Audio.buf[active][ch][0:128)
  │   Master_Audio.buf[!active][ch][0:128) ← memcpy ← DAW Out[0..masterOutCount)
  │   SetEvent(Master_Tick)
  │
  └─ Worker MMCSS thread (WaitForSingleObject(Master_Tick) loop):
      ┌─ HW FIFO 64:   Accumulate 2 ticks → r8brain SRC if SR mismatch → fill 128 → Master_Audio
      ├─ Virtual FIFO 256: Consume 0.5 tick per Master tick (double-buffered IOCTL_WHA_READ/WRITE per cable)
      ├─ Bridge FIFO 128×4: Per bridge, for each ch where masterIn.type==BRIDGE1..4:
      │      sum = 0; nReady=0; for c in 0..3 if ready[c] sum += clientIn[c][active][ch]; nReady++
      │      if nReady>0 mixed = tanh(sum) // soft-clip sum, not avg, keeps loudness
      │      Master_Audio[bridgeSlot] ← mixed;  Master Out → broadcast to clientOut[0..3]
      ├─ Network FIFO PCM 512 / Vorbis 1024: Rx jitter 20/50ms → pop 128 → Master_Audio
      │      Tx: Master_Audio Out → if VORBIS encode 256 frames via libvorbis → sendto UDP payloadBytes
      └─ Loopback: if masterOut[i].type==VIRTUAL && loopback && enabled
           → memcpy(pMasterIn[loopPair] ← pMasterOut[i]) for next tick // +1 tick latency 2.7ms

Why per-thing? HW 64 = low latency for guitar, Virtual 256 = stable when Chrome stutters,
Network 512/1024 = jitter, Bridge 128 (per instance configurable 64–256) = DAW needs may differ.
Changing per-thing buffer = Worker reallocates FIFO, NO ASIOResetRequest. Changing Master asioBuffer/SR → Save & Reset DAW → ASIOResetRequest → DAW restarts engine.
```

No `malloc/lock/printf` in `bufferSwitch()` — only Worker does I/O, SRC, Vorbis, `DeviceIoControl`, `sendto/recvfrom`.

* * *

## 5. Virtual Cable Data — WinHookAudio.sys Loopback Isolation

*   **Driver:** `WDK PortCls WaveRT` — `8× Stereo` (`Virtual 1..8 L/R`) — each cable = **private ring** `64KB = 4096 frames ×4 ×2ch` — isolated: `Virtual1 Out → Virtual1 In only`, no cross-mix.
    
*   **Endpoints Windows sees:** `WinHookAudio Virtual Out 1..8` (Render/Speaker) + `WinHookAudio Virtual In 1..8` (Capture/Mic) — Apps select like any sound card.
    
*   **Data path Pure DLL (Worker-mediated, DAW Must Be Open):**
    
    ```
    Chrome → Windows WASAPI Shared → Driver Render Ring Virtual1 → Worker IOCTL_WHA_READ_VIRTUAL(virtIdx=0, 128) → Master_Audio In[slot where type==VIRTUAL src==0]
    DAW Track → Master_Audio Out[slot where type==VIRTUAL src==0] → Worker IOCTL_WHA_WRITE_VIRTUAL(virtIdx=0, 128) → Driver Capture Ring Virtual1 → OBS/VRChat WASAPI Capture
    ```
    
*   **Loopback (v10 per-slot):** GUI `Loop` column only for `VIRTUAL`. When `Out03 VIRTUAL Virtual3 L Loop=v` and `In03 VIRTUAL Virtual3 L Loop=v` on same `Virtual3 L`, Worker does `memcpy(Master_Audio In03 ← Master_Audio Out03)` next tick **plus** driver write — gives `DAW → Virtual Loopback → VRCT` without extra DAW track (also `DAW → VRCT` monitoring). Latency `+1 tick 2.7ms` per loop.
    
*   **GENERAL alternative:** `Virtual Cables [8 v] Mode [8× Stereo Per-Cable Loopback] Name Prefix [WinHookAudio Virtual ]` — can switch to `1× 64ch` single multi-channel device.
    

* * *

## 6. Bridge Data — 4 Bridges × 4 Clients Shared Channels

*   **Files:** `WinHookAudioBridgeASIO64.dll` one file, `4 CLSIDs B1..B4` → `g_instance 0..3`, plus per-Bridge `clientId 0..3` via `InterlockedIncrement(&bridgeShared->clientCount)` on `init()` — `clientCount 0..4`, 5th `init()` → `ASE_NotPresent` `Bridge1 full (4/4) — use Bridge2/3/4`.
    
*   **Channel mapping (subset):** `Bridge1 getChannels.in = count(masterIn where type==BRIDGE1)` — e.g. `12` of `512` are `BRIDGE1`, `Bridge1` DAW sees exactly `12ch` with names from those slots (`"FL+Live Sum"`). `Bridge1 getChannelInfo(3)` iterates `masterIn` to find 4th `BRIDGE1` slot.
    
*   **Shared mix:** All clients on same Bridge see **same channel set, same names**. Data plane:
    
    ```cpp
    // Slave DAW bufferSwitch (per client)
    clientIn[clientId][active][ch] ← DAW Out[ch]; ready[clientId]=1; WaitForSingleObject(Bridge_Tick[clientId])
    DAW In[ch] ← clientOut[clientId][!active][ch]
    // Worker on Master_Tick per Bridge b in 0..3
    for ch in Bridge chs(b):
      sum=0; n=0; for c in 0..3 if ready[c] { sum+=clientIn[c][active][ch][i]; n++; }
      mixed[ch][i] = n? tanh(sum) : 0; // if lag >3ms → silence that client tick
      Master_Audio[masterSlotForBridgeCh[b][ch]] ← mixed[ch]
      for c in 0..3 clientOut[c][!active][ch] ← Master_Audio Out[masterSlotForBridgeOut[b][ch]] // broadcast
      SetEvent(Bridge_Tick[c]) for c 0..3
    ```
    
*   **Counts:** `4 Bridges ×4 clients = 16 slave DAWs max`, all following `Master general.asioBuffer / sampleRate`. If slave SR differs → Worker `r8brain SRC`. Total pool stays `512 In + 512 Out`.
    

* * *

## 7. Network Data — WHAA My Version Only PCM + Vorbis

*   **Header:** `magic WHAA v5 | codec PCM_F32/PCM_I16/VORBIS | channels | frames | streamId 0..7 | sequence | qpc | payloadBytes + payload` Ports `UDP 6980 audio / 6981 discovery` `WHAA_HELLO broadcast 2s`.
    
*   **Handshake Vorbis:** `WHAA CODEBOOK streamId SR Ch quality headersBytes + 3 Vorbis headers (ident/comment/codebook)` sent `3×` on `Save` when `Tx codec==VORBIS` before first audio — `Rx` caches codebook per `streamId` before `decode`.
    
*   **Per-stream (NETWORK Tab):** `Tx1..8 / Rx1..8` defines `ip:port codec PCM_F32/PCM_I16/VORBIS quality 0.1–1.0 channels 1..32 PCM safe / 1..128 Vorbis bandwidth` → `INPUTS/OUTPUTS slots where type==NETWORK streamId==N ch==i` maps to `Tx/Rx N Ch i`.
    
*   **Data plane:** `Worker Tx: Master_Audio Out → if PCM copy 128*ch*4 → sendto (16KB max 32ch safe MTU); if VORBIS collect 256 frames → libvorbis encode → sendto variable payloadBytes` `Worker Rx: recvfrom nonblock → jitter PCM 20ms / Vorbis 50ms (adaptive Auto) → if VORBIS decode via cached codebook → pop 128 on Master_Tick`.
    
*   **BW example:** `PCM_F32 stereo 48k = 2*48000*4=384 KB/s = 3.0 Mbps`; `Vorbis Q0.4 stereo = ~192 kbps`; `Vorbis Q0.6 64ch = ~1.2 Mbps`; `8 streams can carry 256ch PCM or 512ch Vorbis`. Latency PCM `~23ms LAN` (2.7+20), Vorbis `~58ms WAN` (2.7+10 lookahead+50+5 decode). Firewall rule added by installer.
    

* * *

## 8. GUI Data — 5 Tabs Popup Type 1 (Inside DLL)

**Open:** `Master DAW Open → Preferences → Audio → ASIO → WinHookAudio Master → Configuration → Master DLL controlPanel() → CreateThread → ImGui DX11 1180×720 TopMost`
No `.exe` — `WHAControlPanelUI.cpp` shared, `ListClipper` renders 20 of 512 rows.

*   **INPUTS 512** `masterIn[512]` + **OUTPUTS 512** `masterOut[512]` separate pages because `In01 != Out01` — Filter `All/HW/VIRTUAL/NETWORK/BRIDGE1..4` Search `+Add Input/Output` `+Insert Empty` `Drag :: memmove` `X Delete` `Duplicate` `Loop` column only for `VIRTUAL` `En` keeps index `type==NONE → "- empty -" grey silence`.
    
*   **NETWORK 8** `Tx1..8 / Rx1..8 ip codec Q ch BW Discover Peers`
    
*   **GENERAL (Per-Thing, GLOBAL)** see §4 — `Master Clock SR/Bit/ASIO Buffer → ASIOResetRequest`, Per-Thing `HW64 Virtual256 Bridge1..4 128 Network PCM512 Vorbis1024 Jitter20/50 Auto`, `Virtual Cables 8× Stereo`, `Ports 6980/6981 Fix Firewall`, `MMCSS Exclusive`, `Perf Latency/CPU/Loss/Jitter`
    
*   **ABOUT** Version, `WinHookAudio.sys running`, 5 CLSIDs, `slots.json` path `Export/Import`, `Reset Default`, `Bridge clients: Bridge1 2/4 (FL Client0 + Cubase Client1)` — `Save & Reset DAW → memcpy SHM + FlushViewOfFile + SetEvent(TableChanged) + hostCallback(ASIOResetRequest) → DAW re-calls getChannels/getChannelInfo`.
    

Bridge popup (`FL → Bridge1 → Configuration`) same UI filtered `BRIDGE1` only, `GENERAL` read-only `Follows Master 48000/32/128`.

* * *

## 9. Use Case Plans — Template Configs You Can Import

Each plan is a `slots.json` you can export/import via ABOUT tab. Numbers are `INPUTS / OUTPUTS` indices — keep empty slots to preserve DAW template numbers.

### Plan A — VRChat + VRCT + Discord + OBS Streamer (Loopback) — Most Requested

**Goal:** `VRChat voice + Discord + Desktop → DAW FX → VRChat mic + VRCT captions + OBS stream`, with `4 DAWs sharing Bridge1` for background music production.
| Use | INPUTS (34/512) | OUTPUTS (35/512) | GENERAL Per-Thing | Notes |
| --- | --- | --- | --- | --- |
| HW | In01 HW Scarlett Ch1 `SM58 Mic` In04 HW Scarlett Ch2 `Guitar DI` | Out01 HW Scarlett Out1 `Main L` Out02 HW Out2 `Main R` Out05 HW Headphone `Cue` | HW 64 SR48k Bit32 ASIO128 2.7ms | Low latency for vocal/guitar |
| Virtual | In02 VIRTUAL Virtual1 L `VRChat Out` In05 VIRTUAL Virtual2 L `Discord` In06 VIRTUAL Virtual2 R `Chrome/YouTube` | Out03 VIRTUAL Virtual3 L `To VRCT LOOP` Loop=v Out04 VIRTUAL Virtual1 L `To VRChat Loop` Loop=v | Virtual 256 Driver 8× Stereo | Virtual1 = VRChat Out, Virtual3 = VRCT Loop |
| Loopback | In03 VIRTUAL Virtual3 L `Loopback VRCT LOOP` Loop=v | (paired with In03) | Loop copies Out03→In03 next tick +5.4ms | VRC → DAW → Loopback → VRCT: Track1 `VRChat Out → Gate/Compressor → To VRCT LOOP` → In03 monitor Track |
| Bridge Shared | In07 BRIDGE1 Ch1 `FL+Live Sum` In08 BRIDGE1 Ch2 `FL+Live Sum R` (shared 2/4 clients) | Out10 BRIDGE1 Ch1 `To FL+Live` Out11 BRIDGE1 Ch2 `To FL+Live R` (broadcast to 4) | Bridge1 128 Bridge2..4 128 | FL Studio Client0 + Ableton Live Client1 both select `Bridge1` same 16ch, summed to In07, Master DAW beat FX, broadcast back |
| Network | In10 NETWORK Rx1 Ch1 `Laptop Vorbis` Q0.4 | Out12 NETWORK Tx1 Ch1 `To Laptop PCM` | PCM512 Vorbis1024 Jitter20/50 | Optional laptop co-stream PCM LAN |
| Empty | In09 NONE `- empty -` In12 NONE `- empty -` (keep In10 network at 10, Out12 at 12) | Out08 NONE `- empty -` |  | Preserves template: In07 always Beat, In10 always Laptop |

**Master DAW Template (Reaper 16 tracks):**

```
T01 VRChat Mic: In01 SM58 → ReaGate → ReaComp → Out01 Main L + Out04 To VRChat Loop (sidechain) + Out03 To VRCT Loop
T02 Discord: In05 Discord → Duck T01 (-12dB) → Out01 Main
T03 Beat: In07 FL+Live Sum → EQ → Out01 Main (summed FL+Cubase.Live on same ch)
T04 VRCT Monitor: In03 Loopback VRCT → Meter only → Out01 (verify captions receive)
T05 Laptop: In10 Laptop Vorbis → Out01
```

**Result:** `VRChat hears compressed mic + beat`, `VRCT receives DAW-processed VRChat via Loopback Virtual3`, `OBS captures Virtual3`, `FL and Live jam on same Bridge1 16ch shared`.

* * *

### Plan B — Music Production Hub (4 Bridges × 4 DAWs = 16 DAWs Shared) + Network PCM

**Goal:** Producer runs Master DAW (Reaper 512) as mixer, 4 teams each on own Bridge sharing channels, plus network to studio B PCM 32ch multitrack.
| Use | INPUTS (80/512) | OUTPUTS (80/512) | GENERAL |
| --- | --- | --- | --- |
| HW | In01-08 HW `Scarlett 1..8 Mic/Line` In09-16 HW `ADAT 1..8` | Out01-08 HW `Scarlett 1..8` | HW 64 Low |
| Virtual | In17 VIRTUAL Virtual4 L `Splice` In18 VIRTUAL Virtual5 `Ref Track` | Out17 VIRTUAL Virtual4 `To Splice` | Virtual 256 |
| Bridge1 Team Beat (4 DAWs shared) | In20-23 BRIDGE1 Ch1..4 `Beat Team Sum` `Kick Sum` etc shared 4/4 (FL+Cubase+Live+Bitwig on Bridge1 same 16ch) | Out20-23 BRIDGE1 Ch1..4 `To Beat Team` broadcast | Bridge1 128 (beat needs tight) |
| Bridge2 Team Vocal (4 DAWs) | In24-27 BRIDGE2 Ch1..4 `Vocal Team Sum` shared 4/4 | Out24-27 BRIDGE2 `To Vocal Team` | Bridge2 128 |
| Bridge3 Team Mix (2 DAWs) | In28-29 BRIDGE3 Ch1..2 `Mix Sum` shared 2/4 | Out28-29 BRIDGE3 `To Mix` | Bridge3 256 (mix can be looser) |
| Bridge4 Free | In30 BRIDGE4 Ch1 `Free` 0/4 | Out30 BRIDGE4 | Bridge4 128 |
| Network PCM | In40-71 NETWORK Rx1 PCM_F32 Ch1..32 `StudioB PCM 32ch` In72-75 Rx2 VORBIS Q0.6 Ch1..4 `Remote Vocal` | Out40-71 NETWORK Tx1 PCM_F32 Ch1..32 `To StudioB 32ch` Out72-75 Tx2 VORBIS `To Remote` | Network PCM512 Jitter20 Vorbis50 |
| Empty | Every 8th slot `- empty -` to group (In16, In32...) | Same | Keep groups visually separated |

**Data:** `Master_Audio 16MB holds 80ch active of 512`, `Bridge Shared 8MB×4 but only 20ch of Bridge used → Worker sum only that subset → 0.05ms tick`. `Network PCM 32ch×128×4=16KB per packet × 375 ticks/s = 6 MB/s = 48 Mbps LAN` safe.

* * *

### Plan C — Network 512 Channel Film Score WAN (Vorbis)

**Goal:** `PC-A Main (Master 512)` + `PC-B Remote Orchestra (Master 512)` WAN via Vorbis `Q0.6` to carry `~100ch` over `~3 Mbps` instead of PCM `~150 Mbps`.
| Use | INPUTS | OUTPUTS | GENERAL + NETWORK |
| --- | --- | --- | --- |
| PC-A Master | In01 HW `Main Mic` In02-09 BRIDGE `Local DAWs` In10-71 NETWORK Rx1 VORBIS 64ch `Remote Orch 64ch Q0.6` In72-103 Rx2 VORBIS 32ch `Choir 32ch Q0.4` | Out01 HW `Main` Out10-71 NETWORK Tx1 VORBIS 64ch `To Remote Mix 64ch Q0.6` Out72 NETWORK Tx3 PCM_F32 `Click PCM` (low latency click) | ASIO128 SR48k Vorbis1024 Jitter50 Auto |
| PC-B Remote | Mirrored: In01-64 HW `Orch Mic 64ch` → Tx1 VORBIS 64ch Q0.6 → PC-A Rx1 | In01-64 NETWORK Rx1 VORBIS `From Main Mix` → HW | Same |

**Why Vorbis:** `PCM 64ch = 64*48000*4 = 12.2 MB/s = 98 Mbps` per stream → 2 streams `196 Mbps` saturates WAN. `Vorbis Q0.6 64ch ≈ 64*~19kbps = 1.2 Mbps` → 8 streams `~10 Mbps` feasible. Click track kept `PCM_F32` low latency, rest `VORBIS` quality.

* * *

### Plan D — Live Performer Minimal (8+8, One Bridge 16ch)

**Goal:** Guitarist needs `HW Guitar + Mic → DAW AmpSim → HW Main + Virtual To Discord + One friend DAW Bridge loopback jam`.
| INPUTS (10/512) | OUTPUTS (10/512) | GENERAL |
| --- | --- | --- |
| In01 HW `Guitar DI` In02 HW `Vocal SM58` In03 VIRTUAL Virtual1 `Backing Track` In04 BRIDGE1 Ch1 `Friend Jam` shared 1/4 | Out01 HW `Main L` Out02 HW `Main R` Out03 VIRTUAL Virtual1 `To Discord LOOP` Loop=v Out04 BRIDGE1 Ch1 `To Friend` | HW 64 (guitar needs 1.3ms) Virtual 256 Bridge1 128 ASIO128 2.7ms total RTL ~4ms |

`Loopback Virtual1`: Backing track from Chrome → Virtual1 → DAW Amp → Discord Loopback.

* * *

### Plan E — Template Preservation with Empty Slots (512 Channel Film Template)

**Goal:** Keep `In01-08 Drums, In09-16 Bass, In17-24 Guitars, In25-32 Vocals` even when not used, so `Reaper Template Track 25 = Vocal` never shifts.

```
INPUTS 512 Filter: All

In01 HW `Kick` In02 HW `Snare` In03 HW `Hat` In04 HW `Tom1` In05 HW `Tom2` In06 HW `OH L` In07 HW `OH R` In08 NONE `- empty -` (separator)
In09 VIRTUAL Virtual1 `Bass DI` In10 NONE `- empty -` In11 NONE `- empty -` In12 NONE `- empty -` In13 NONE `- empty -` In14 NONE `- empty -` In15 NONE `- empty -` In16 NONE `- empty -` (reserve 8 for Bass group)
In17 HW `Guitar L` ... In24 NONE ...
In25 BRIDGE1 `Lead Vocal Sum` shared 4/4 ... In32 BRIDGE1 `Choir Sum`

+Insert Empty Above In09 to add new bass mic without shifting In17 guitars.
Drag In03 Hat to In05 position = memmove INPUT indices, OUTPUTS untouched.
```

Empty slots count in `masterInCount` but `enabled=false` → `getChannelInfo` `"- empty -"` grey, `Worker` fills `0.0f`, DAW shows but silent.

* * *

## 10. Performance & Limits

*   `512ch × 128 frames × 4B ×2 ping-pong = 16MB SHM` → `memcpy 256KB/tick ~0.05ms SIMD` per Master_Tick 2.7ms.
    
*   `Bridge sum tanh(sum) soft-clip` — `2 DAWs at -6dB → 0dB`, average would be -6dB quiet. If client lag >3ms → silence that client tick, others unaffected.
    
*   `Loopback copy = +1 tick 2.7ms` per VIRTUAL loop, no kernel copy.
    
*   `4× Bridge ×4 clients = 16 slave DAWs max` on same PC, same `Master Clock`. DAWs cap display: Reaper 512, Cubase 256, Ableton 256, FL 128.
    
*   `Network BW: PCM 32ch=6 Mbps/stereo, Vorbis 64ch Q0.4 ~1.2 Mbps compressed — 8 streams can carry 256ch PCM or 512ch Vorbis via Vorbis`.
    

* * *

## 11. Registry & Install Data

**WinHookAudio.reg 5 entries:** `Master + Bridge1..4 CLSIDs` point to `C:\Program Files\WinHookAudio\*.dll` — same Bridge file, 4 keys. **Inno Setup:** 3 files + `.cat` + firewall `netsh advfirewall firewall add rule name="WinHookAudio" dir=in action=allow protocol=UDP localport=6980-6981` + `slots.json` import. Requires `EV Cert + Microsoft Attestation` for Win11 normal mode, else `bcdedit /set testsigning on`.

* * *

## 12. Export

This v10.1 is the **Data Architecture** companion to **v10 Datasheet**. Save both as `WinHookAudio_Datasheet_v10.md` + `WinHookAudio_Datasheet_v10.1_Data_Architecture.md`.
_End of Datasheet v10.1 — Unified 512 In+512 Out | IN/OUT Split | Movable Empty Loopback | Per-Thing GENERAL | Pure DLL WHAA PCM/Vorbis | 4× Bridge Shared 16 DAWs | Data Architecture & Plans_