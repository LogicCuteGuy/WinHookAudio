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
constexpr uint32_t kBridgeRing = 6;       // blocks kept each way; the delay is at most kBridgeRing - 1
#define WHA_BRIDGE_CLIENTS wha::kBridgeClients
#define WHA_BRIDGE_CHANNELS wha::kBridgeChannels
#define WHA_BRIDGE_FRAMES wha::kBridgeFrames

// Blocks are numbered. Master block t (one Master Clock tick): the Worker writes the Bridge's Master
// OUT slots into toClients[t % kBridgeRing], then sets masterBlocks = t + 1 and wakes the clients. A
// client runs one period per block, in order, catching up after bunched ticks: block m's input from
// toClients[m % ring], its output into fromClient[its place][m % ring], then clientBlocks = m + 1. At
// block t the Worker mixes each client's block t - delay (BridgeDelayBlocks): a client that is late by
// less than the delay loses nothing, instead of a lost or repeated block per late tick.
#pragma pack(push, 1)
struct WHABridgeShared {
  // Client place i: the process ID of the app holding it, 0 = free. Claimed with a compare-exchange on
  // init, given back when that app closes the driver; a place whose process has exited is taken back.
  volatile int32_t owner[WHA_BRIDGE_CLIENTS] = {};
  // 64-bit counters at 8-byte offsets (16 = after owner[]): whole reads and writes on x64.
  volatile int64_t masterBlocks = 0;                     // blocks the Worker has published
  volatile int64_t clientBlocks[WHA_BRIDGE_CLIENTS] = {};  // blocks a client has produced; -1 = not running
  volatile int64_t clientLate[WHA_BRIDGE_CLIENTS] = {};    // blocks mixed as silence: the client was late
  volatile int64_t clientSkipped[WHA_BRIDGE_CLIENTS] = {}; // blocks a client skipped after falling a ring behind
  float fromClient[WHA_BRIDGE_CLIENTS][kBridgeRing][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
  float toClients[kBridgeRing][WHA_BRIDGE_CHANNELS][WHA_BRIDGE_FRAMES] = {};
};
#pragma pack(pop)
static_assert(offsetof(WHABridgeShared, masterBlocks) % 8 == 0 && offsetof(WHABridgeShared, clientBlocks) % 8 == 0,
              "Bridge block counters must be 8-byte aligned");

// How many blocks a Bridge client's output trails the Master (its GENERAL Bridge buffer on top of one
// block): 1 + ceil(bridgeBuffer / asioBuffer), 2..kBridgeRing - 1. Also what the client reports as
// output latency, in blocks.
constexpr int BridgeDelayBlocks(uint32_t bridgeBuffer, uint32_t asioBuffer) {
  const uint32_t extra = asioBuffer ? (bridgeBuffer + asioBuffer - 1) / asioBuffer : 1;
  const uint32_t delay = 1 + extra;
  return delay < 2 ? 2 : delay > kBridgeRing - 1 ? static_cast<int>(kBridgeRing - 1) : static_cast<int>(delay);
}

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
  b.clientBlocks[clientId] = -1;
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
static_assert(sizeof(WHABridgeShared::fromClient) == WHA_BRIDGE_CLIENTS * kBridgeRing * WHA_BRIDGE_CHANNELS * WHA_BRIDGE_FRAMES * sizeof(float),
              "fromClient size");
static_assert(sizeof(WHABridgeShared::toClients) == kBridgeRing * WHA_BRIDGE_CHANNELS * WHA_BRIDGE_FRAMES * sizeof(float),
              "toClients size");
static_assert(sizeof(WHABridgeShared) > 7 * 1024 * 1024, "Bridge shared ~7.9MB (fits its 8MB view)");
static_assert(BridgeDelayBlocks(128, 128) == 2 && BridgeDelayBlocks(256, 128) == 3 && BridgeDelayBlocks(1024, 64) == 5,
              "Bridge delay: 1 + buffer in blocks, 2..kBridgeRing - 1");
static_assert(kBridgeClients == 4, "kBridgeClients 4");
static_assert(kBridgeChannels == 64, "kBridgeChannels 64");
static_assert(kBridgeFrames == 1024, "kBridgeFrames 1024");

}  // namespace wha
