# WinHookAudio Datasheet — FINAL v10

**Brand:** WinHookAudio  
**Concept:** DAW = Man in the Middle — Master DAW MUST be open to have sound (by design)  
**Architecture:** Pure DLL (No Server, No ControlPanel.exe) — Popup Type 1 inside `controlPanel()`  
**Unified Pool:** **512 absolute per direction** `512 In + 512 Out` — Not `512+256`. Bridge is a `Type` inside 512  
**Version:** FINAL v10 — Loopback + 4-Way Shared Bridge — 2026-09-14

* * *

## 1. Overview

WinHookAudio is a custom Windows audio framework to connect everything through the Master DAW.

*   **Master DAW** (Reaper/Ableton/Cubase) is the **Mixer + Effects Rack**. All routing is done with DAW faders / VSTs.
    
*   **Master ASIO Driver** exposes up to **512 inputs / 512 outputs** to the Master DAW. Channels are **fully dynamic** (`1..512`) with custom names (`32 chars`) via `getChannelInfo()`.
    
*   **4x Bridge ASIO Drivers** expose **subsets** of the unified 512 pool to slave DAWs on the same PC. Each Bridge now supports **4 DAWs sharing the same channels** (summed mix) → `4 Bridges × 4 clients = 16 slave DAWs max`.
    
*   **Virtual Cable** (`WinHookAudio.sys`) exposes `8× Stereo` (16ch) endpoints to Windows (Chrome/Discord/OBS/VRChat). Each VIRTUAL slot has a **Loopback** toggle per slot for `VRC → DAW → Virtual Loopback → VRCT` monitoring.
    
*   **Audio Network** (My Version Only, magic `WHAA`, UDP `6980` audio / `6981` discovery) streams **PCM Float32 / Int16 and Vorbis** (per-stream `Q 0.1–1.0`) to another PC running WinHookAudio.
    
*   **Close Master DAW = Silence** for Virtual / Bridge / Network — intentional, by design.
    

* * *

## 2. Key Features

| Feature | Spec |
| --- | --- |
| **Unified Pool** | **512 In + 512 Out max** absolute. `BRIDGE1..4` is `Type` inside `masterIn[512]` / `masterOut[512]`. e.g. `In: 200 HW + 100 Virtual + 100 Network + 112 Bridge1..4 = 512` |
| **IN/OUT Split** | `INPUTS 512` and `OUTPUTS 512` are **separate pages**. Index `In01 != Out01`. Reordering `In04` does NOT move `Out04`. |
| **Movable Empty Slot** | `SLOT_NONE` = `"- empty -"` silence. Keeps DAW template channel numbers stable. Drag `::` to move, `+Insert Empty Above/Below`, `+Add`, `X` delete, `Duplicate`. |
| **Loopback (NEW v10)** | Per-`VIRTUAL` slot **Loop** checkbox in INPUTS/OUTPUTS. `Worker` copies `Master OUT → Master IN` next tick. Example: `VRChat → Virtual1 → DAW → Virtual3 LOOP → VRCT` ~5.4ms loop. |
| **Shared Bridge (NEW v10)** | Per Bridge supports **4 DAWs sharing same ch set** (summed mix with `tanh` soft-clip). `Bridge1` can be opened by `FL + Cubase + Live + DAW4` at same time, same `16ch`. |
| **Dynamic Channels** | `masterInCount` / `masterOutCount` `1..512` dynamic. `+Add Input` → `hostCallback(ASIOResetRequest)` → DAW re-queries `getChannels()` / `getChannelInfo()`. |
| **Custom Names** | `name[32]` per slot — `SM58 Mic`, `Discord`, `FL Out L`, `To VRCT` appear in DAW `Track Input:` |
| **Per-Thing Buffers** | Buffers are **per category in GENERAL**, not per slot: `Master ASIO 128` / `HW 64` / `Virtual 256` / `Bridge1..4 128` / `Network PCM 512 / Vorbis 1024` + `Jitter PCM 20ms / Vorbis 50ms` |
| **2 Drivers → 5 Devices** | `1× Master 512` + `4× Bridge` (same `Bridge.dll` file, 4 CLSIDs, subsets of unified 512). Each Bridge 4 clients. |
| **GUI** | **Popup Type 1 only** (`controlPanel()` → `CreateThread` → ImGui DX11). `INPUTS 512 | OUTPUTS 512 | NETWORK 8 | GENERAL (Per-Thing) | ABOUT` |
| **Network Codecs** | **My Version Only** `WHAA` — `PCM_FLOAT32` (0 ms, LAN), `PCM_INT16` (half BW), `VORBIS` (30–50 ms, WAN, `Q 0.1–1.0`, `64–500 kbps` per stereo) per stream. |

* * *

## 3. System Architecture — DAW is Man in the Middle

```
                          MASTER DAW — Man in Middle — MUST BE OPEN
                          Device: "WinHookAudio Master" [In:1..512 | Out:1..512]
   +-- bufferSwitch(128) = MASTER CLOCK 2.7ms@48k/32Float ---------------------------------+
   | DAW Mixer + VST — all routing here                                                     |
   | In02 Virtual1 L "VRChat Out"  → Track1 + Gate → Out03 Virtual3 L "To VRCT" [LOOP=v]  |
   | In04 Bridge1 Ch1 "FL+Live Sum" (2 clients summed) → Track2 + EQ → Out01 HW "Main L"  |
   +-- memcpy via Shared Memory -----------------------------------------------------------+
                          ^                                                       |
                          | memcpy  Worker Thread inside Master DLL              | memcpy
             +------------------------------------------------------------------------+
             | WinHookAudioMasterASIO64.dll — Holder Worker Thread (MMCSS Pro Audio) |
             |  NOT in bufferSwitch thread — never blocks DAW                        |
             |  ├─ WASAPI Exclusive KS — hwBuffer 64 → Real HW (Scarlett/Realtek)    |
             |  ├─ DeviceIoControl IOCTL_WHA_READ/WRITE — virtualBuffer 256 → WinHookAudio.sys 8× Stereo |
             |  │   └─ LOOPBACK: if Master OUT loopback=v → copy to Master IN next tick |
             |  ├─ UDP 6980 WHAA — networkPcmBuffer 512 / vorbis 1024 → Jitter 20/50ms + libvorbis |
             |  └─ memcpy — bridgeBuffer 128 → 4× Bridge Shared SHM (sum 4 clients / broadcast) |
             +------------------------------------------------------------------------+
                |              |                   |              |     |     |     |
              [HW]    [Virtual.sys 8× Stereo]   [Network WHAA] [B1]  [B2]  [B3]  [B4]
                |              |          PCM/Vorbis Q0.4     sum4  sum4  sum4  sum4
                v              v                  v             v     v     v     v
             Scarlett  Chrome/Discord/    Laptop 192.168.1.50  FL  Cubase Live DAW4
                       OBS VRChat Loop    6980 PCM/Vorbis    Bridge1 4 clients shared 16ch
                       VRCT captures Virtual3 Loopback       Same ch, summed mix
```

*   `Master DLL bufferSwitch()`: `DAW In ← SHM In` + `SHM Out ← DAW Out` + `SetEvent(Master_Tick)` — only `memcpy`.
    
*   `Worker WaitForSingleObject(Master_Tick)`: `for(i<masterInCount) if(NONE) silence else if(HW) KS else if(VIRTUAL) IOCTL (+ loopback copy) else if(NETWORK) jitter.pop else if(BRIDGE) sum Bridge clients`
    
*   `Bridge DLL`: `Slave DAW bufferSwitch() ↔ clientIn[myId] ↔ Worker sum → Master In` / `Master Out → clientOut[0..3] broadcast → all 4 clients`
    
*   `Slave getBufferSize()==Master general.asioBuffer`, `init()==ASE_NotPresent` if Master not open. If 5th client on same Bridge → `ASE_NotPresent` `Bridge1 full (4/4) — use Bridge2/3/4`.
    

* * *

## 4. File & Build Structure — Only 3 Binaries

```
WinHookAudio/
├── common/  # Single source shared by ALL
│   ├── WHASlotTable.h        # v10 80KB — truth (loopback + shared)
│   ├── WHASharedMemory.h     # Global\WinHookAudio_* names & helpers
│   ├── WHAAudioSHM.h         # 16MB ping-pong + 4× 8MB Bridge Shared
│   ├── WHAPacket.h           # WHAA Header + Vorbis CODEBOOK handshake
│   └── WHAControlPanelUI.cpp # ONE ImGui code — Master & Bridge
│
├── asio-master/  # WinHookAudioMasterASIO64.dll (x64 ~950KB + ImGui + libvorbis)
│   ├── DllMain.cpp            # DllGetClassObject, DllRegisterServer
│   ├── WinHookMasterASIO.cpp  # class WinHookMasterASIO : public IASIO
│   ├── MasterHolder.cpp       # Worker Thread MMCSS — holds all + loopback + shared sum
│   └── MasterControlPanel.cpp # controlPanel() → CreateThread → UI
│
├── asio-bridge/  # WinHookAudioBridgeASIO64.dll (x64 ~600KB) — ONE FILE
│   ├── DllMain.cpp            # 4 CLSIDs B1..B4 → g_instance 0..3 → + clientId 0..3
│   └── WinHookBridgeASIO.cpp  # class WinHookBridgeASIO : public IASIO (subset, 4-way shared)
│
├── driver/  # WinHookAudio.sys (WDK PortCls WaveRT ~200KB)
│   ├── WHAAdapter.cpp / WHAMiniportWaveRT.cpp / WHAStream.cpp
│   └── WinHookAudio.inf/.cat  # Root\WinHookAudio
│
└── installer/
    ├── WinHookAudio.reg  # 5 CLSIDs
    └── WinHookAudio.iss  # Inno Setup + Firewall 6980-6981 + Attestation
```

**Installed:** `C:\Program Files\WinHookAudio\WinHookAudio.sys + WinHookAudioMasterASIO64.dll + WinHookAudioBridgeASIO64.dll`
**Registry:**

```reg
HKLM\SOFTWARE\ASIO\WinHookAudio Master     CLSID_Master  → Master DLL (512)
HKLM\SOFTWARE\ASIO\WinHookAudio Bridge 1   CLSID_B1      → Bridge DLL (subset BRIDGE1, 4 clients)
HKLM\SOFTWARE\ASIO\WinHookAudio Bridge 2   CLSID_B2      → Bridge DLL (subset BRIDGE2, 4 clients)
HKLM\SOFTWARE\ASIO\WinHookAudio Bridge 3   CLSID_B3      → Bridge DLL
HKLM\SOFTWARE\ASIO\WinHookAudio Bridge 4   CLSID_B4      → Bridge DLL
```

**Dependencies:** `ASIO SDK 2.3.3` + `WDK 11 22621` + `ImGui 1.90 + Win32 + DX11` + `libvorbis 1.3.7 + libogg 1.3.5` + `r8brain-free` + `moodycamel::ReaderWriterQueue`

* * *

## 5. Heart — WHASlotTable.h v10

```cpp
#pragma pack(push,1)
#define WHA_MAX 512
#define WHA_BRIDGE_COUNT 4
#define WHA_NAME_LEN 32
#define WHA_NET_STREAMS 8

enum WHASlotType : uint32_t {
  SLOT_NONE=0,        // "- empty -" silence — keeps index stable
  SLOT_HW=1,          // Real Mic/Speaker via WASAPI KS
  SLOT_VIRTUAL=2,     // WinHookAudio.sys 8× Stereo
  SLOT_NETWORK=3,     // WHAA PCM/Vorbis
  SLOT_BRIDGE1=4, SLOT_BRIDGE2=5, SLOT_BRIDGE3=6, SLOT_BRIDGE4=7
};

struct WHASlot {
  WHASlotType type;
  int32_t  srcChannel; // 0..N inside type/stream
  int32_t  streamId;   // NETWORK only 0..7
  char     name[WHA_NAME_LEN]; // shown in DAW via getChannelInfo()
  bool     enabled;    // false = silence but keeps index
  bool     loopback;   // v10 NEW — only valid if type==VIRTUAL — Worker copies OUT→IN
  uint8_t  _pad[2];
};

struct WHAGeneral {
  uint32_t sampleRate = 48000; // GLOBAL Master Clock
  uint32_t bitDepth = 32;      // GLOBAL ASIOSTFloat32LSB
  uint32_t asioBuffer = 128;   // GLOBAL DAW sees this — 2.7ms
  uint32_t hwBuffer = 64;                // Per-Thing Worker FIFO
  uint32_t virtualBuffer = 256;
  uint32_t bridgeBuffer[4] = {128,128,128,128};
  uint32_t networkPcmBuffer = 512;
  uint32_t networkVorbisBuffer = 1024;
  uint32_t jitterPcm = 20;      // ms
  uint32_t jitterVorbis = 50;   // ms
};

struct WHANetworkStream {
  char ip[16]; // "192.168.1.50"
  uint16_t port = 6980;
  enum { PCM_F32, PCM_I16, VORBIS } codec = PCM_F32;
  float quality = 0.4f; // 0.1..1.0 Vorbis
  uint32_t channels = 2;
};

struct WHASlotTable {
  uint32_t version;
  uint32_t masterInCount;  // 1..512 dynamic — INPUTS Tab
  WHASlot  masterIn[512];  // Input index 0 = DAW Input 1
  uint32_t masterOutCount; // 1..512 dynamic — OUTPUTS Tab (independent)
  WHASlot  masterOut[512]; // Output index 0 = DAW Output 1
  WHAGeneral general;
  WHANetworkStream netTx[8], netRx[8]; // 8 Tx + 8 Rx My Version Only
};
#pragma pack(pop)
// %ProgramData%\WinHookAudio\slots.json + SHM Global\WinHookAudio_SlotTable 80KB
```

**Total system:** `512 In + 512 Out` max. `BRIDGE1..4` are types inside `masterIn/Out[512]`. Shared Bridge does NOT add extra pool.

* * *

## 6. Shared Memory — Created ONLY by Master DLL (v10 Shared)

| Name | Size | Created by | Content |
| --- | --- | --- | --- |
| `Global\WinHookAudio_SlotTable` | 80 KB | Master `init()` | `WHASlotTable v10` |
| `Global\WinHookAudio_Master_Audio` | 16 MB | Master | `float[2][512][4096]` ping-pong |
| `Global\WinHookAudio_Bridge1_Shared` | 8 MB | Master | `WHABridgeShared` — 4 clients `clientIn[4][2][64][4096] + clientOut[4][2][64][4096] + mixed[2][64][4096] + ready[4] + clientCount` |
| `Bridge2/3/4_Shared` | 8 MB ×3 | Master | Same for Bridge 2..4 |
| `Global\WinHookAudio_Master_Tick` | Event | Master | `bufferSwitch() → Worker` — **Master Clock** |
| `Bridge1_Tick[4]...Bridge4_Tick[4]` | Event ×16 | Worker | `Worker → Bridge clients 0..3` broadcast |
| `Global\WinHookAudio_TableChanged` | Event | Master | `Save → Worker reload` |

Close Master DAW = all SHM dies = silence (by design).
**WHABridgeShared (per Bridge):**

```cpp
struct WHABridgeShared {
  volatile LONG clientCount; // 0..4 InterlockedIncrement on Open
  volatile LONG ready[4];
  volatile LONG activeBuf[4];
  float clientIn[4][2][64][4096];  // Slave DAW OUT → Master (per client)
  float clientOut[4][2][64][4096]; // Master OUT → Slave DAW (broadcast)
  float mixedIn[2][64][4096];       // Worker sum of 4 clients
};
```

* * *

## 7. ASIO Drivers — 5 Devices from 2 Files

**Master:** `getChannels(*in=masterInCount,*out=masterOutCount)` / `getChannelInfo(){ WHASlot &s=isInput?masterIn[ch]:masterOut[ch]; strcpy(name,s.type==NONE?"- empty -":s.name); }`
**Bridge 1..4 (shared 4-way):** `DllGetClassObject(CLSID_B1){ g_instance=0; }` → `init()` does `InterlockedIncrement(&bridgeShared->clientCount) → clientId 0..3 (if >4 → ASE_NotPresent)` / `getChannels()` counts `masterIn where type==BRIDGE1+g_instance` / `getChannelInfo()` iterates to nth `BRIDGE1+g_instance` / `bufferSwitch()` writes `clientIn[clientId][active]` and waits on `Bridge_Tick[clientId]` which Worker broadcasts.
**Shared mix:** Worker collects `ready[0..3]` with `3ms timeout` per Bridge, sums `sum+=clientIn[clientId][active][ch]`, `mixed = tanh(sum)` soft-clip → `pMasterIn[masterSlot]`.

* * *

## 8. GUI — Popup Type 1 Only (Inside DLL)

**Open:** `Master DAW Open → Preferences → Audio → ASIO → WinHookAudio Master → Configuration → Master DLL controlPanel() → CreateThread → ImGui DX11 1180×720 TopMost dark #1E1E1E`

```cpp
ASIOError controlPanel(){ if(gPopup) return ASE_OK; gPopup=CreateThread(0,0,PopupThread,pTable,0,NULL); return ASE_OK; }
// PopupThread: copy to editCopy → ImGui loop with ListClipper (512 rows → 20 drawn) → Save → memcpy SHM + slots.json + hostCallback(ASIOResetRequest)
```

No `ControlPanel.exe`. Bridge popup (`FL Studio → Bridge 1 → Configuration`) uses same `WHAControlPanelUI.cpp` filtered to `BRIDGE1` only, `GENERAL` read-only `Follows Master`.

### Tab 1 — INPUTS 512 (masterIn[512] only)

```
INPUTS: [34/512]  Bridge1:2/4 clients  Filter:[All/HW/VIRTUAL/NETWORK/BRIDGE1..4] Search:[____] [+Add Input] [+Insert Empty]
:: | #  | Type    | Source      | Name in DAW    | Loop | En | X
:: |In01| HW     | Scarlett Ch1| SM58 Mic     |      | v | x   drag :: = memmove INPUT only
:: |In02| VIRTUAL| Virtual1 L  | VRChat Out   |      | v | x
:: |In03| VIRTUAL| Virtual3 L  | Loopback VRCT|  [v] | v | x   LOOP=v — loopback enabled
:: |In04| BRIDGE1| Bridge1 Ch1 | FL+Live Sum  |      | v | x   shared 2/4 clients summed
:: |In05| NONE   | -           | - empty -    |      |   | x   empty keeps index, silence, grey in DAW
Right-click In04 → [Insert Empty Above/Below] [Duplicate] [Delete]
Drag In04 → In02 = memmove swap — ListClipper 20/512 — +Add appends "- empty -"
```

### Tab 2 — OUTPUTS 512 (masterOut[512] only — independent indices)

```
OUTPUTS: [35/512] Filter Search [+Add Output] [+Insert Empty]
:: | #   | Type    | Source      | Name in DAW | Loop | En | X
:: |Out03| VIRTUAL| Virtual3 L  | To VRCT   | [v] | v | x  LOOP=v pairs with In03 → Worker copies Out03→In03 next tick
:: |Out10| BRIDGE1| Bridge1 Ch1 | To FL+Live|     | v | x  broadcast to 4 clients
Moving Out03 does NOT move In03. Separate indices.
```

**Loop column:** only enabled when `Type==VIRTUAL`, grey otherwise. `In03 LOOP` + `Out03 LOOP` on same `Virtual3 L` creates full loopback `DAW → Virtual Ring → Loopback → DAW`.
**Shared badge:** `Bridge1:2/4` shows active clients in `ABOUT` tab too: `Bridge1: FL (Client0) + Cubase (Client1) active — 2/4`.

### Tab 3 — NETWORK 8 (My Version Only WHAA)

```
Tx1 [192.168.1.50:6980] [PCM_F32 v]/[PCM_I16]/[VORBIS] Q[0.4 v] Ch[32 v] BW 6.1 Mbps [v]
Tx2 [203.0.113.5:6980]  [VORBIS v] Q[0.6 v] Ch[64 v] BW 1.2 Mbps [v] → 512ch via 8 streams
Rx1 [192.168.1.50 v] [VORBIS v] Q0.4 Ch2 192kbps [v] → linked to INPUTS where Type=NETWORK Rx1
Discovery 6981 [ON] Peers: [WinHookAudio-PC-B found] — PCM→Vorbis triggers CODEBOOK handshake
     Header: WHAA v5 codec channels frames streamId sequence qpc payloadBytes + pcm/vorbisData
     Handshake: WHAA CODEBOOK streamId SR Ch quality headersBytes + 3 Vorbis headers
```

### Tab 4 — GENERAL (Per-Thing, GLOBAL — not per slot)

```
1. MASTER CLOCK (DAW sees via getSampleRate/getBufferSize) [Save & Reset DAW → ASIOResetRequest]
 Sample Rate: [48000 v] 44100 48000 96000
 Bit Depth:   [32 Float v] 16 24 32 Float ASIOSTFloat32LSB fixed
 ASIO Buffer: [128 v] 64 128 256 512 1024 2.7ms @128/48k — ONE tick for all 512ch

2. PER-THING WORKER BUFFERS (internal FIFO — Worker adapts to Master Clock, no DAW reset on change)
 Hardware (KS Exclusive):    [64 v]
 Virtual Cable:              [256 v]
 Bridge1:[128 v] Bridge2:[128 v] Bridge3:[256 v] Bridge4:[128 v]
 Network PCM:[512 v] Vorbis:[1024 v] Jitter PCM:[20ms v][x]Auto Vorbis:[50ms v][x]Auto

 [x]MMCSS Pro Audio [x]Exclusive   Virtual Cables: [8 v] 8× Stereo  Name[WinHookAudio Virtual ]
 Ports: Audio 6980 Discovery 6981 [Fix Firewall]  Perf: In 2.7ms CPU 0.4% Loss 0% Jitter 18ms
```

Worker FIFO: `HW 64 collect 2× → 128` / `Virtual 256 collect 0.5× → 128` / `Network 512 jitter → 128`
Bridge `GENERAL` read-only `Follows Master 48000/32/128`.

### Tab 5 — ABOUT

Version, `WinHookAudio.sys` running, 5 CLSIDs, `%ProgramData%\WinHookAudio\slots.json`, `Export/Import`, Firewall `UDP 6980-6981 [Fix]`, `Reset Default`, `Bridge clients: Bridge1 2/4`.
**Save:** `editCopy → *pTable, version++, WriteFile slots.json, FlushViewOfFile, SetEvent(TableChanged), theHostCallback(ASIOResetRequest) → DAW re-calls getChannels/getChannelInfo → "- empty -" grey, loopback active, new bridge clients re-count`.

* * *

## 9. Virtual Cable — WinHookAudio.sys

MSVAD WaveRT, `8× Stereo` (`16ch`) `Virtual 1..8 L/R`, each `64KB RingBuffer` `DeviceIoControl IOCTL_WHA_READ/WRITE/SET_LOOPBACK`. `INPUTS Tab VIRTUAL Virtual1 L = VRChat Out` ↔ Windows Sound `VRChat Output → WinHookAudio Virtual 1`. `OBS/VRCT captures Virtual 3`. Loopback flow: `VRChat → Virtual1 Ring → Worker IOCTL → Master SHM In02 → DAW Track → Master SHM Out03 LOOP=v → Worker memcpy → Master SHM In03 (loopback) + Virtual3 Ring → VRCT` loop `~5.4ms (2×128)` without extra DAW track. Alternative mode in GENERAL: `1× 64ch` single device.

* * *

## 10. Network — WHAA My Version Only (PCM + Vorbis)

```
Header: magic WHAA v5 codec PCM_F32/PCM_I16/VORBIS channels frames streamId sequence qpc payloadBytes + payload
Handshake: WHAA CODEBOOK streamId SR Ch quality headersBytes + 3 Vorbis headers (ident/comment/codebook)
Ports: UDP 6980 audio, 6981 discovery WHAA_HELLO broadcast 2s
Worker Tx: SHM Out → encode (Vorbis needs 256 frames) → sendto
Worker Rx: recvfrom nonblock → jitter PCM 20ms / Vorbis 50ms → decode → pop on Master_Tick
Latency: PCM ~23ms LAN, Vorbis ~58ms WAN — Worker does r8brain SRC if remote SR differs
Bandwidth: PCM 32ch=6 Mbps/stereo, Vorbis 64ch Q0.4 ~1.2 Mbps compressed — 8 streams can carry 256ch PCM or 512ch Vorbis
Firewall: netsh advfirewall firewall add rule name="WinHookAudio" dir=in action=allow protocol=UDP localport=6980-6981
```

Not VBAN compatible — magic `WHAA` only. Per-stream codec `Q` in `NETWORK Tab`.

* * *

## 11. Registry & Install

**WinHookAudio.reg:** 5 entries pointing to same Bridge file; **Inno Setup:** 3 files + `.cat` + firewall + `bcdedit /set testsigning off` requires EV Cert + Microsoft Attestation for Win11 normal mode.

* * *

## 12. Performance & Limits

*   `512ch × 128 frames × 4 B × 2 ping-pong = 16 MB SHM` — `memcpy 256KB/tick ~0.05ms SIMD`, no `malloc/lock/printf` in `bufferSwitch()`.
    
*   `Shared Bridge sum: tanh(sum) soft-clip` — `2 DAWs at -6dB → 0dB`, not average. If client lag >3ms → silence that client tick.
    
*   `Loopback copy: memcpy OUT→IN next tick` — `+1 tick 2.7ms` per loop, no kernel copy.
    
*   `4× Bridge ×4 clients = 16 slave DAWs max` on same PC, same `Master Clock`.
    
*   DAWs cap display: Reaper 512, Cubase 256, Ableton 256, FL 128.
    
*   Thread: `Master bufferSwitch()` real-time, `Worker MMCSS Pro Audio`, `Network Rx/Tx`, `UI`.
    

* * *

## 13. Example Routing — VRChat + Shared Bridge + Network

```
# Master DAW Template (Reaper)
In02 Virtual1 L "VRChat Out" (HW KS not used) → Track1 "VRChat + Gate" → Out03 Virtual3 L "To VRCT LOOP" (loops to In03)
In03 Virtual3 L "Loopback VRCT LOOP" → Track2 "Monitor" → Out01 HW "Main L"
In04 Bridge1 Ch1 "FL+Live Sum" (FL Client0 + Live Client2 summed) → Track3 "Beat + EQ" → Out10 Bridge1 Ch1 "To FL+Live" (broadcast to 4)
In10 Network Rx1 Ch1 "Laptop Mic VORBIS Q0.4" → Track4 → HW

# Slave DAWs (same Bridge1 16ch shared)
FL Studio → Bridge1 Client0 → Out01 "To Master" → summed to In04
Cubase → Bridge1 Client1 → Out01 "To Master" → summed to In04 (same ch)

# Network PC-B
PC-B Master DAW In17 "From PC-A VORBIS" → HW
```

* * *

_End of Datasheet FINAL v10 — Unified 512 In+512 Out | IN/OUT Split Index | Movable Empty Loopback | Per-Thing GENERAL | Pure DLL WHAA PCM/Vorbis | 4× Bridge Shared 16 DAWs_