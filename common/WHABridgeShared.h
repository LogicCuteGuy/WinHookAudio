#pragma once

// WinHookAudio Bridge shared region — offline header-only.
// Vocabulary: Shared Bridge, Slot, Slot Table.
// No audio, no devices, no threads here — pure layout and offline validation.
// Master Driver creates SHM, Bridge Driver opens it. 4 clients per Bridge max.

#include <cstddef>
#include <cstdint>
#include <cmath>

#include "WHASlotTable.h"

#define WHA_BRIDGE_CLIENTS 4
#define WHA_BRIDGE_CHANNELS 64
#define WHA_BRIDGE_FRAMES 4096
#define WHA_BRIDGE_BUFFERS 2

namespace wha {

#pragma pack(push, 1)
struct WHABridgeShared {
  volatile int32_t clientCount = 0;  // 0..4 InterlockedIncrement on init
  volatile int32_t ready[WHA_BRIDGE_CLIENTS] = {};
  volatile int32_t activeBuf[WHA_BRIDGE_CLIENTS] = {};
  float clientIn[WHA_BRIDGE_CLIENTS][WHA_BRIDGE_BUFFERS][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  float clientOut[WHA_BRIDGE_CLIENTS][WHA_BRIDGE_BUFFERS][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  float mixedIn[WHA_BRIDGE_BUFFERS][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  volatile int32_t mixedActive = 0;
};
#pragma pack(pop)

// ---- Offline validation (no threads, no SHM) ----

constexpr bool IsValidBridgeClientCount(int32_t count) { return count >= 0 && count <= WHA_BRIDGE_CLIENTS; }

inline bool TryAddBridgeClient(WHABridgeShared& b, int32_t* outClientId) {
  if (b.clientCount >= WHA_BRIDGE_CLIENTS) return false;
  if (outClientId) *outClientId = b.clientCount;
  ++b.clientCount;
  return true;
}

constexpr bool IsValidBridgeReady(int32_t v) { return v == 0 || v == 1; }
constexpr bool IsValidBridgeActiveBuf(int32_t v) { return v == 0 || v == 1; }

// Soft-clip sum preserves loudness: tanh(sum) not average.
// Two DAWs at -6dB (0.5) => sum 1.0 => tanh(1.0)=0.761 not 0.5 average.
inline float SoftClipMix(float sum) { return std::tanh(sum); }

inline float MixBridgeClients(const float* samples, const int32_t* ready, int nClients) {
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

// Compile-time layout invariants.
static_assert(WHA_BRIDGE_CLIENTS == 4, "Four clients per Bridge");
static_assert(WHA_BRIDGE_CHANNELS == 64, "64 channels per Bridge");
static_assert(WHA_BRIDGE_FRAMES == 4096, "4096 frames per Bridge buffer");
static_assert(sizeof(WHABridgeShared::clientIn) == WHA_BRIDGE_CLIENTS * WHA_BRIDGE_BUFFERS * WHA_BRIDGE_CHANNELS * WHA_BRIDGE_FRAMES * sizeof(float),
              "clientIn size");
static_assert(sizeof(WHABridgeShared::clientOut) == WHA_BRIDGE_CLIENTS * WHA_BRIDGE_BUFFERS * WHA_BRIDGE_CHANNELS * WHA_BRIDGE_FRAMES * sizeof(float),
              "clientOut size");
static_assert(sizeof(WHABridgeShared) > 8 * 1024 * 1024, "Bridge shared at least 8MB");

}  // namespace wha
