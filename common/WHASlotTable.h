#pragma once

// WinHookAudio Slot Table ABI v10.1 — offline header-only first slice.
// Vocabulary: Slot, Slot Table, Master Clock, Loopback, Shared Bridge, Network Stream.
// No audio, no devices, no network, no pointers across the ABI boundary.
// C++20 user-mode; fixed-width types; WDK-compatible layout (packed, POD structs).

#include <cstddef>
#include <cstdint>
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
  // Per-thing Worker FIFOs — Worker adapts to Master Clock, no DAW reset.
  uint32_t hwBuffer = 64;
  uint32_t virtualBuffer = 256;
  uint32_t bridgeBuffer[WHA_BRIDGE_COUNT] = {128, 128, 128, 128};
  uint32_t networkPcmBuffer = 512;
  uint32_t networkVorbisBuffer = 1024;
  uint32_t jitterPcm = 20;
  uint32_t jitterVorbis = 50;
};

struct WHANetworkStream {
  char ip[16] = {};
  uint16_t port = 6980;
  WHACodec codec = WHA_PCM_F32;
  float quality = 0.4f;
  uint32_t channels = 2;
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
};
#pragma pack(pop)

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
