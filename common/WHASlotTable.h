#pragma once

// WinHookAudio Slot Table ABI v10.1 — offline header-only first slice.
// Vocabulary: Slot, Slot Table, Master Clock, Loopback, Shared Bridge, Network Stream.
// No audio, no devices, no network, no pointers across the ABI boundary.
// C++20 user-mode; fixed-width types; WDK-compatible layout (packed, POD structs).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wha {
constexpr uint32_t kMax = 512;
constexpr uint32_t kBridgeCount = 4;
constexpr uint32_t kNameLen = 32;
constexpr uint32_t kNetStreams = 8;
constexpr uint32_t kMasterClockRateDefault = 48000u;
constexpr uint32_t kMasterClockBitsDefault = 32u;
constexpr uint32_t kMasterClockBufferDefault = 128u;
constexpr uint32_t kMaxPcmChannels = 32;
constexpr uint32_t kMaxVorbisChannels = 128;
constexpr uint32_t kEndpointIdLen = 64;  // "{0.0.0.00000000}.{guid}" is 55 chars

// Backward compat macros (prefer constexpr above)
#define WHA_MAX wha::kMax
#define WHA_BRIDGE_COUNT wha::kBridgeCount
#define WHA_NAME_LEN wha::kNameLen
#define WHA_NET_STREAMS wha::kNetStreams
#define WHA_MASTER_CLOCK_RATE_DEFAULT wha::kMasterClockRateDefault
#define WHA_MASTER_CLOCK_BITS_DEFAULT wha::kMasterClockBitsDefault
#define WHA_MASTER_CLOCK_BUFFER_DEFAULT wha::kMasterClockBufferDefault

enum WHASlotType : uint32_t {
  SLOT_NONE = 0,
  SLOT_HW = 1,
  SLOT_VIRTUAL = 2,
  SLOT_NETWORK = 3,
  SLOT_BRIDGE1 = 4,
  SLOT_BRIDGE2 = 5,
  SLOT_BRIDGE3 = 6,
  SLOT_BRIDGE4 = 7
};

enum WHACodec : uint32_t { WHA_PCM_F32 = 0, WHA_PCM_I16 = 1, WHA_VORBIS = 2 };

#pragma pack(push, 1)
struct WHASlot {
  WHASlotType type = SLOT_NONE;
  int32_t srcChannel = 0;
  int32_t streamId = 0;
  char name[WHA_NAME_LEN] = {};
  uint8_t enabled = 0;   // 0/1 fixed-width (was bool)
  uint8_t loopback = 0;  // 0/1 fixed-width (was bool)
  uint8_t _pad[2] = {};
};

struct WHAGeneral {
  // Master Clock — what Master DAW sees via getSampleRate/getBufferSize.
  uint32_t sampleRate = WHA_MASTER_CLOCK_RATE_DEFAULT;
  uint32_t bitDepth = WHA_MASTER_CLOCK_BITS_DEFAULT;
  uint32_t asioBuffer = WHA_MASTER_CLOCK_BUFFER_DEFAULT;
  // Per-thing Worker FIFOs — Worker adapts to Master Clock, no DAW reset. Except hwBuffer: the HW
  // device period, applied when the Worker opens the device, and part of the reported latencies.
  // A request below the device's minimum period gets the minimum (the reported latency follows the
  // period actually used); 0 = Auto = the minimum.
  uint32_t hwBuffer = 64;
  uint32_t virtualBuffer = 256;
  uint32_t bridgeBuffer[WHA_BRIDGE_COUNT] = {128, 128, 128, 128};
  uint32_t networkPcmBuffer = 512;
  uint32_t networkVorbisBuffer = 1024;
  uint32_t jitterPcm = 20;
  uint32_t jitterVorbis = 50;
  uint32_t virtualCables = 8;  // 8 or 64 (GENERAL Virtual Cables)
  char virtualName[32] = "WinHookAudio Virtual";
  // HW slot endpoints (IMMDevice IDs); empty = the Windows default device of that direction.
  char hwRenderId[kEndpointIdLen] = "";
  char hwCaptureId[kEndpointIdLen] = "";
};

struct WHANetworkStream {
  char ip[16] = {};
  uint16_t port = 6980;
  WHACodec codec = WHA_PCM_F32;
  float quality = 0.4f;
  uint32_t channels = 2;
};

// HW devices per direction: device 0 is GENERAL hwRenderId / hwCaptureId (the output one is the
// Master Clock), devices 1..3 are WHAHwMore, the rest WHAHwExtra ("" = not in the list). A HW slot's
// streamId is its device. Both are appended (hwMore after netRx, hwExtra after cables), so every
// earlier field keeps its offset. Read them through HwDeviceId, never by array index.
// kHwDevices is as large as the 80 KB Slot Table mapping comfortably holds (128 bytes per device
// place): more devices than a PC has, so the PC's CPU and bus are the real limit.
constexpr int32_t kHwDevices = 128;
constexpr int32_t kHwMoreDevices = 3;  // devices 1..3 (the first append)
struct WHAHwMore {
  char renderId[kHwMoreDevices][kEndpointIdLen] = {};
  char captureId[kHwMoreDevices][kEndpointIdLen] = {};
};
constexpr int32_t kHwExtraDevices = kHwDevices - 1 - kHwMoreDevices;  // devices 4 and up
struct WHAHwExtra {
  char renderId[kHwExtraDevices][kEndpointIdLen] = {};
  char captureId[kHwExtraDevices][kEndpointIdLen] = {};
};

// How the Worker opens a HW device, per device and direction (appended after hwExtra; 0 in older
// tables = Exclusive, the behaviour before modes). Exclusive: WASAPI Exclusive, straight to the driver,
// lowest latency, no other app can use the device meanwhile. Shared: through the Windows mixer, so
// other apps keep playing / recording; more latency, the device format is the mixer's (Windows
// converts the rate). Auto: Exclusive, else Shared (the device refuses exclusive mode: not allowed in
// its Windows settings, or no exclusive format at the Master Clock's rate). A device another app holds
// exclusively opens in neither.
enum WHAHwMode : uint8_t { HW_MODE_EXCLUSIVE = 0, HW_MODE_SHARED = 1, HW_MODE_AUTO = 2 };
constexpr uint8_t kHwModeCount = 3;
struct WHAHwModes {
  uint8_t render[kHwDevices] = {};
  uint8_t capture[kHwDevices] = {};
};

// Per Virtual Cable: its channels (2, 4, 6, 8) and sample format (WHASampleKind: 0 = 32-bit float,
// 1 = 16, 2 = 24, 3 = 32-bit PCM, 4 = 24 bits in 32). The rate is the Master Clock's. The Worker sends
// them to the driver, whose endpoints then offer exactly this format. Appended after hwMore.
constexpr int32_t kVirtualSlotCables = 8;
struct WHACableSetting {
  uint8_t channels = 2;
  uint8_t format = 0;
  uint8_t _pad[2] = {};
};

struct WHASlotTable {
  uint32_t version = 0;
  uint32_t masterInCount = 0;
  WHASlot masterIn[WHA_MAX] = {};
  uint32_t masterOutCount = 0;
  WHASlot masterOut[WHA_MAX] = {};
  WHAGeneral general = {};
  WHANetworkStream netTx[WHA_NET_STREAMS] = {};
  WHANetworkStream netRx[WHA_NET_STREAMS] = {};
  WHAHwMore hwMore = {};
  WHACableSetting cables[kVirtualSlotCables] = {};
  WHAHwExtra hwExtra = {};
  WHAHwModes hwModes = {};
};

// Every HW device's endpoint ID and mode, by device index: a copy of the Slot Table's lists (the
// Worker's "requested" snapshot).
struct WHAHwDeviceIds {
  char renderId[kHwDevices][kEndpointIdLen] = {};
  char captureId[kHwDevices][kEndpointIdLen] = {};
  WHAHwModes modes = {};
};
#pragma pack(pop)

inline void TruncateCopy(char* dst, std::size_t dstLen, const char* src);

// Endpoint ID of HW device `device` of one direction (see WHAHwMore); nullptr out of range.
inline const char* HwDeviceId(const WHASlotTable& t, bool isInput, int device) {
  if (device == 0) return isInput ? t.general.hwCaptureId : t.general.hwRenderId;
  if (device < 0 || device >= kHwDevices) return nullptr;
  if (device <= kHwMoreDevices) return isInput ? t.hwMore.captureId[device - 1] : t.hwMore.renderId[device - 1];
  const int e = device - 1 - kHwMoreDevices;
  return isInput ? t.hwExtra.captureId[e] : t.hwExtra.renderId[e];
}
inline char* HwDeviceId(WHASlotTable& t, bool isInput, int device) {
  return const_cast<char*>(HwDeviceId(static_cast<const WHASlotTable&>(t), isInput, device));
}
inline const char* HwDeviceId(const WHAHwDeviceIds& ids, bool isInput, int device) {
  if (device < 0 || device >= kHwDevices) return nullptr;
  return isInput ? ids.captureId[device] : ids.renderId[device];
}
// Mode of HW device `device` of one direction (WHAHwMode); out of range or unknown = Exclusive.
inline WHAHwMode HwDeviceMode(const WHAHwModes& m, bool isInput, int device) {
  if (device < 0 || device >= kHwDevices) return HW_MODE_EXCLUSIVE;
  const uint8_t v = isInput ? m.capture[device] : m.render[device];
  return v < kHwModeCount ? static_cast<WHAHwMode>(v) : HW_MODE_EXCLUSIVE;
}
inline WHAHwMode HwDeviceMode(const WHASlotTable& t, bool isInput, int device) { return HwDeviceMode(t.hwModes, isInput, device); }
inline WHAHwMode HwDeviceMode(const WHAHwDeviceIds& ids, bool isInput, int device) { return HwDeviceMode(ids.modes, isInput, device); }
inline void SetHwDeviceMode(WHASlotTable& t, bool isInput, int device, uint8_t mode) {
  if (device < 0 || device >= kHwDevices) return;
  (isInput ? t.hwModes.capture : t.hwModes.render)[device] = mode;
}
// Device 0 is always in the list ("" = the Windows default); the others when they have an ID.
inline bool HwDeviceListed(const WHASlotTable& t, bool isInput, int device) {
  const char* id = HwDeviceId(t, isInput, device);
  return id && (device == 0 || id[0]);
}
inline void CopyHwDeviceIds(const WHASlotTable& t, WHAHwDeviceIds& out) {
  for (int d = 0; d < kHwDevices; ++d) {
    TruncateCopy(out.renderId[d], kEndpointIdLen, HwDeviceId(t, false, d));
    TruncateCopy(out.captureId[d], kEndpointIdLen, HwDeviceId(t, true, d));
  }
  out.modes = t.hwModes;
}
inline void ClearHwMoreDevices(WHASlotTable& t) {  // every device but 0 out of the lists (with its mode)
  t.hwMore = WHAHwMore{};
  t.hwExtra = WHAHwExtra{};
  for (int d = 1; d < kHwDevices; ++d) t.hwModes.render[d] = t.hwModes.capture[d] = HW_MODE_EXCLUSIVE;
}
// A HW slot's device: its streamId; one out of range is device 0.
inline int HwDeviceOf(const WHASlot& s) { return s.streamId > 0 && s.streamId < kHwDevices ? s.streamId : 0; }

// ---- Offline validation (no I/O, no threads) ----

constexpr bool IsValidCount(uint32_t count) { return count >= 1 && count <= WHA_MAX; }

constexpr bool IsBridgeType(WHASlotType type) {
  return type == SLOT_BRIDGE1 || type == SLOT_BRIDGE2 || type == SLOT_BRIDGE3 ||
         type == SLOT_BRIDGE4;
}

// Bridge types live inside the 512 pool, not as extra pool.
// Validates slot index for bridge types (index must be <512).
constexpr bool IsBridgeInsidePool(WHASlotType type, uint32_t index) {
  if (!IsBridgeType(type)) return true;
  return index < kMax;
}
constexpr bool IsBridgeInsidePool(WHASlotType type) {
  if (!IsBridgeType(type)) return true;
  return type >= SLOT_BRIDGE1 && type <= SLOT_BRIDGE4;
}

// Empty Slots count but stay silent.
constexpr bool IsSilentSlot(const WHASlot& slot) {
  return slot.type == SLOT_NONE || slot.enabled == 0;
}

// Loopback is meaningful only on Virtual Cable Slots and defaults off.
constexpr bool IsLoopbackValid(const WHASlot& slot) {
  if (slot.loopback && slot.type != SLOT_VIRTUAL) return false;
  return true;
}

constexpr bool IsValidSlotEnabled(uint8_t v) { return v == 0 || v == 1; }
constexpr bool IsValidSlotLoopback(uint8_t v) { return v == 0 || v == 1; }

constexpr bool IsValidCodec(uint32_t codec) {
  return codec == WHA_PCM_F32 || codec == WHA_PCM_I16 || codec == WHA_VORBIS;
}

constexpr bool IsValidNetworkChannels(WHACodec codec, uint32_t channels) {
  if (channels < 1) return false;
  if (codec == WHA_VORBIS) return channels <= kMaxVorbisChannels;
  return channels <= kMaxPcmChannels;
}

inline void TruncateCopy(char* dst, std::size_t dstLen, const char* src) {
  if (dstLen == 0) return;
  if (src == nullptr) {
    dst[0] = '\0';
    for (std::size_t i = 1; i < dstLen; ++i) dst[i] = '\0';
    return;
  }
  std::size_t i = 0;
  for (; i + 1 < dstLen && src[i] != '\0'; ++i) dst[i] = src[i];
  dst[i] = '\0';
  for (++i; i < dstLen; ++i) dst[i] = '\0';
}

constexpr bool IsValidMasterClock(uint32_t rate, uint32_t buffer) {
  const bool rateOk = rate == 44100 || rate == 48000 || rate == 96000;
  const bool bufferOk =
      buffer == 64 || buffer == 128 || buffer == 256 || buffer == 512 || buffer == 1024;
  return rateOk && bufferOk;
}

// What the Master DAW sees through getChannels/getChannelInfo/getSampleRate/getBufferSize/
// getLatencies. A change here needs hostCallback(ASIOResetRequest). The HW devices and period count
// too: the Worker opens them at start, and they set the reported latencies. Other Per-Thing buffers
// and routing fields do not.
inline bool DawVisibleChanged(const WHASlotTable& a, const WHASlotTable& b) {
  if (a.masterInCount != b.masterInCount || a.masterOutCount != b.masterOutCount) return true;
  if (a.general.sampleRate != b.general.sampleRate || a.general.asioBuffer != b.general.asioBuffer) return true;
  if (a.general.hwBuffer != b.general.hwBuffer) return true;
  for (int d = 0; d < kHwDevices; ++d)
    if (std::strncmp(HwDeviceId(a, false, d), HwDeviceId(b, false, d), kEndpointIdLen) != 0 ||
        std::strncmp(HwDeviceId(a, true, d), HwDeviceId(b, true, d), kEndpointIdLen) != 0)
      return true;
  if (std::memcmp(&a.hwModes, &b.hwModes, sizeof(WHAHwModes)) != 0) return true;  // the Worker opens devices in their mode
  // A HW slot moved to another device may need that device opened (and changes its channel name).
  // A Bridge or Virtual slot's channel is in its automatic name ("Bridge1 Ch3", "Virtual 2 R").
  auto slotDiffers = [](const WHASlot& x, const WHASlot& y) {
    return x.type != y.type || x.enabled != y.enabled || std::strncmp(x.name, y.name, kNameLen) != 0 ||
           (x.type == SLOT_HW && HwDeviceOf(x) != HwDeviceOf(y)) ||
           ((IsBridgeType(x.type) || x.type == SLOT_VIRTUAL) && x.srcChannel != y.srcChannel);
  };
  for (uint32_t i = 0; i < a.masterInCount && i < kMax; ++i)
    if (slotDiffers(a.masterIn[i], b.masterIn[i])) return true;
  for (uint32_t i = 0; i < a.masterOutCount && i < kMax; ++i)
    if (slotDiffers(a.masterOut[i], b.masterOut[i])) return true;
  return false;
}

// HW slots take one channel of their device: srcChannel 0 = L (Ch 1), 1 = R (Ch 2), then Ch 3 ... The
// Worker opens each device with all its channels (its Windows device format, WHAEndpointChannels.h)
// or, if it refuses that in exclusive mode, stereo; a channel past what the device opened is silent.
constexpr int32_t kHwMaxChannels = 64;  // MADI-class interfaces

// VIRTUAL slots take one channel of a Virtual Cable: srcChannel = cable * 8 + channel (0 = L, 1 = R,
// then C, LFE, ... up to the cable's channel count), cables 1..8. A channel past the cable's count is
// silent. (Files from before 8-channel cables stored cable * 2 + side; DeserializeSlots converts them.)
constexpr int32_t kVirtualCableChannels = 8;
inline int VirtualCableOf(const WHASlot& s) {
  return s.srcChannel < 0 ? 0 : (s.srcChannel / kVirtualCableChannels) % kVirtualSlotCables;
}
inline int VirtualChannelOf(const WHASlot& s) { return s.srcChannel < 0 ? 0 : s.srcChannel % kVirtualCableChannels; }
constexpr bool IsValidCableSetting(const WHACableSetting& c) {
  return (c.channels == 2 || c.channels == 4 || c.channels == 6 || c.channels == 8) && c.format <= 4;
}

// A name nobody chose: what an empty slot shows.
inline bool IsPlaceholderName(const char* name) {
  return name == nullptr || name[0] == '\0' || std::strncmp(name, "- empty -", kNameLen) == 0;
}

// A device's friendly name without the driver part, short enough for "<name> R" in an ASIO channel
// name: "Microphone (High Definition Audio Device)" -> "Microphone". Empty in, empty out.
inline void ShortDeviceName(const char* friendly, char* out, std::size_t outLen) {
  constexpr std::size_t kMaxShort = kNameLen - 3;  // room for " L" and the terminator
  std::size_t n = 0;
  if (friendly)
    while (friendly[n] && n < kMaxShort && !(friendly[n] == ' ' && friendly[n + 1] == '(')) ++n;
  if (n >= outLen) n = outLen - 1;
  if (friendly && n) std::memcpy(out, friendly, n);
  out[n] = '\0';
}

// The name a slot gets when its source is assigned and nobody named it: what it carries, so the DAW's
// channel list reads "Microphone L" instead of "- empty -". `slots` is the slot's list (INPUTS or
// OUTPUTS); a Bridge slot's channel is its order among that Bridge's slots (see MasterHolder).
// hwDevices: friendly names of this direction's HW devices, by device index (kHwDevices entries;
// nullptr or a nullptr/"" entry = unknown: "HW In L", "HW In 2 L").
inline void AutoSlotName(const WHASlot* slots, [[maybe_unused]] uint32_t count, uint32_t index, bool isInput,
                         const char* const* hwDevices, char* out) {
  const WHASlot& s = slots[index];
  const int ch = s.srcChannel + 1;
  switch (s.type) {
    case SLOT_NONE: std::snprintf(out, kNameLen, "- empty -"); break;
    case SLOT_HW: {
      const int d = HwDeviceOf(s);
      char device[kNameLen];
      ShortDeviceName(hwDevices ? hwDevices[d] : nullptr, device, sizeof(device));
      if (!device[0] && d == 0) std::snprintf(device, sizeof(device), "HW %s", isInput ? "In" : "Out");
      else if (!device[0]) std::snprintf(device, sizeof(device), "HW %s %d", isInput ? "In" : "Out", d + 1);
      if (s.srcChannel == 0 || s.srcChannel == 1)
        std::snprintf(out, kNameLen, "%s %c", device, s.srcChannel == 0 ? 'L' : 'R');
      else
        std::snprintf(out, kNameLen, "%s Ch%d", device, ch);
      break;
    }
    case SLOT_VIRTUAL: {  // "Virtual 2 L", "Virtual 2 R", then "Virtual 2 Ch3" (which speaker depends on the cable)
      const int c = VirtualChannelOf(s);
      if (c < 2) std::snprintf(out, kNameLen, "Virtual %d %c", VirtualCableOf(s) + 1, c ? 'R' : 'L');
      else std::snprintf(out, kNameLen, "Virtual %d Ch%d", VirtualCableOf(s) + 1, c + 1);
      break;
    }
    case SLOT_NETWORK: std::snprintf(out, kNameLen, "%s%d Ch%d", isInput ? "Rx" : "Tx", s.streamId + 1, ch); break;
    default:  // SLOT_BRIDGE1..4: the channel the slot picks in the Bridge app
      std::snprintf(out, kNameLen, "Bridge%d Ch%d", static_cast<int>(s.type - SLOT_BRIDGE1) + 1, ch);
      break;
  }
}

// The channel name a DAW gets (getChannelInfo): "- empty -" for an empty slot, the slot's own name,
// or, when nobody named it, its automatic name. Automatic names are not stored, so they follow the
// slot's type, source and order.
inline void DawChannelName(const WHASlot* slots, uint32_t count, uint32_t index, bool isInput,
                           const char* const* hwDevices, char* out) {
  const WHASlot& s = slots[index];
  if (s.type != SLOT_NONE && !IsPlaceholderName(s.name)) TruncateCopy(out, kNameLen, s.name);
  else AutoSlotName(slots, count, index, isInput, hwDevices, out);
}

inline void SetSlotName(WHASlot& slot, const char* text) {
  TruncateCopy(slot.name, WHA_NAME_LEN, text);
}

// Compile-time ABI invariants.
static_assert(WHA_MAX == 512, "Slot pool must be 512 absolute per direction");
static_assert(WHA_BRIDGE_COUNT == 4, "Four Bridges required");
static_assert(WHA_NAME_LEN == 32, "Slot names are 32 chars");
static_assert(WHA_NET_STREAMS == 8, "Eight Tx plus eight Rx streams");
static_assert(sizeof(WHASlot) == 48, "WHASlot packed size must be stable");
static_assert(sizeof(WHASlotTable) >= 49000 && sizeof(WHASlotTable) < 81920,
              "Control plane stays in 80KB class");

}  // namespace wha
