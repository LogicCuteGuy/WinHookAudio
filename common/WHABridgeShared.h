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
  // Client place i: the process ID of the app holding it, 0 = free. Claimed with a compare-exchange on
  // init, given back when that app closes the driver; a place whose process has exited is taken back.
  volatile int32_t owner[WHA_BRIDGE_CLIENTS] = {};
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

// Apps holding a place on this Bridge (0..4).
inline int32_t CountBridgeClients(const WHABridgeShared& b) {
  int32_t n = 0;
  for (uint32_t i = 0; i < kBridgeClients; ++i) n += b.owner[i] != 0 ? 1 : 0;
  return n;
}

// Offline, single-threaded place bookkeeping (tests). The Bridge Driver claims and frees places in
// SHM with InterlockedCompareExchange on owner[] (WinHookBridgeASIO::claimClientPlace).
inline bool TryAddBridgeClient(WHABridgeShared& b, int32_t ownerPid, int32_t* outClientId) {
  for (uint32_t i = 0; i < kBridgeClients; ++i) {
    if (b.owner[i] != 0) continue;
    b.owner[i] = ownerPid;
    if (outClientId) *outClientId = static_cast<int32_t>(i);
    return true;
  }
  return false;
}
inline void RemoveBridgeClient(WHABridgeShared& b, int32_t clientId) {
  if (clientId < 0 || clientId >= static_cast<int32_t>(kBridgeClients)) return;
  b.ready[clientId] = 0;
  b.owner[clientId] = 0;
}

// ---- Bridge channels ----
// A BRIDGE(n) slot picks its channel in the Bridge app by srcChannel (0 = Ch 1), so moving rows
// never renumbers the app's channels. -1: not a Bridge slot, or a channel out of range.
inline int BridgeChannelOf(const WHASlot& s) {
  return IsBridgeType(s.type) && s.srcChannel >= 0 && s.srcChannel < static_cast<int32_t>(kBridgeChannels) ? s.srcChannel : -1;
}

// A Bridge app always gets a stereo pair (FL Studio refuses fewer than 2 outputs); more when a slot
// picks a higher channel. Unrouted channels are silent.
constexpr long kMinBridgeChannels = 2;
inline long BridgeChannelCount(const WHASlot* slots, uint32_t count, WHASlotType type) {
  long n = kMinBridgeChannels;
  for (uint32_t i = 0; i < count; ++i)
    if (slots[i].type == type && BridgeChannelOf(slots[i]) >= n) n = BridgeChannelOf(slots[i]) + 1;
  return n;
}

// The first slot of `type` on Bridge channel `ch`: its index, or -1 when no slot picks it.
inline int FindBridgeSlot(const WHASlot* slots, uint32_t count, WHASlotType type, int ch) {
  for (uint32_t i = 0; i < count; ++i)
    if (slots[i].type == type && BridgeChannelOf(slots[i]) == ch) return static_cast<int>(i);
  return -1;
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
