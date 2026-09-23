#pragma once

// WinHookAudio Bridge shared region — offline header-only.
// Vocabulary: Shared Bridge, Slot, Slot Table.
// No audio, no devices, no threads here — pure layout and offline validation.
// Master Driver creates SHM, Bridge Driver opens it. 4 clients per Bridge max.

#include <cstddef>
#include <cstdint>
#include <cmath>

#include "WHASlotTable.h"

namespace wha {
constexpr uint32_t kBridgeClients = 4;
constexpr uint32_t kBridgeChannels = 64;
constexpr uint32_t kBridgeFrames = 1024;  // = largest valid ASIO/Bridge buffer (IsValidMasterClock)
constexpr uint32_t kBridgeBuffers = 2;
#define WHA_BRIDGE_CLIENTS wha::kBridgeClients
#define WHA_BRIDGE_CHANNELS wha::kBridgeChannels
#define WHA_BRIDGE_FRAMES wha::kBridgeFrames
#define WHA_BRIDGE_BUFFERS wha::kBridgeBuffers

#pragma pack(push, 1)
struct WHABridgeShared {
  volatile int32_t clientCount = 0;  // 0..4 InterlockedIncrement on init (POD for SHM)
  volatile int32_t ready[WHA_BRIDGE_CLIENTS] = {};
  volatile int32_t activeBuf[WHA_BRIDGE_CLIENTS] = {};
  float clientIn[WHA_BRIDGE_CLIENTS][WHA_BRIDGE_BUFFERS][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  float clientOut[WHA_BRIDGE_CLIENTS][WHA_BRIDGE_BUFFERS][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  float mixedIn[WHA_BRIDGE_BUFFERS][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  volatile int32_t mixedActive = 0;
};
#pragma pack(pop)

// ---- Offline validation (no threads, no SHM) ----

constexpr bool IsValidBridgeClientCount(int32_t count) { return count >= 0 && count <= static_cast<int32_t>(kBridgeClients); }

// SHM-safe: caller must use InterlockedCompareExchange for cross-process atomicity.
// Offline helper uses single-threaded increment (tests only, not SHM).
inline bool TryAddBridgeClient(WHABridgeShared& b, int32_t* outClientId) {
  if (b.clientCount >= static_cast<int32_t>(kBridgeClients)) return false;
  if (outClientId) *outClientId = b.clientCount;
  ++b.clientCount;
  return true;
}

inline bool TryAddBridgeClientAtomic(WHABridgeShared& b, int32_t* outClientId) {
  // Requires Windows.h InterlockedCompareExchange when used in SHM context.
  // Offline fallback: same as TryAddBridgeClient (single-threaded).
  return TryAddBridgeClient(b, outClientId);
}

constexpr bool IsValidBridgeReady(int32_t v) { return v == 0 || v == 1; }
constexpr bool IsValidBridgeActiveBuf(int32_t v) { return v == 0 || v == 1; }

// Soft-clip sum preserves loudness: tanh(sum) not average.
// Two DAWs at -6dB (0.5) => sum 1.0 => tanh(1.0)=0.761 not 0.5 average.
inline float SoftClipMix(float sum) { return std::tanh(sum); }

inline float MixBridgeClients(const float* samples, const volatile int32_t* ready, int nClients) {
  float sum = 0.0f;
  int nReady = 0;
  for (int i = 0; i < nClients; ++i) {
    if (ready[i]) {
      sum += samples[i];
      ++nReady;
    }
  }
  if (nReady == 0) return 0.0f;
  return SoftClipMix(sum);
}
inline float MixBridgeClients(const float* samples, const int32_t* ready, int nClients) {
  return MixBridgeClients(samples, reinterpret_cast<const volatile int32_t*>(ready), nClients);
}

// Compile-time layout invariants.
static_assert(WHA_BRIDGE_CLIENTS == 4, "Four clients per Bridge");
static_assert(WHA_BRIDGE_CHANNELS == 64, "64 channels per Bridge");
static_assert(WHA_BRIDGE_FRAMES == 1024, "1024 frames per Bridge buffer");
static_assert(sizeof(WHABridgeShared::clientIn) == WHA_BRIDGE_CLIENTS * WHA_BRIDGE_BUFFERS * WHA_BRIDGE_CHANNELS * WHA_BRIDGE_FRAMES * sizeof(float),
              "clientIn size");
static_assert(sizeof(WHABridgeShared::clientOut) == WHA_BRIDGE_CLIENTS * WHA_BRIDGE_BUFFERS * WHA_BRIDGE_CHANNELS * WHA_BRIDGE_FRAMES * sizeof(float),
              "clientOut size");
static_assert(sizeof(WHABridgeShared) > 4 * 1024 * 1024, "Bridge shared ~4.5MB");
static_assert(kBridgeClients == 4, "kBridgeClients 4");
static_assert(kBridgeChannels == 64, "kBridgeChannels 64");
static_assert(kBridgeFrames == 1024, "kBridgeFrames 1024");

}  // namespace wha
