#pragma once

// WHANetworkEngine — WHAA Network Streams for the Worker (ADR 0003, ADR 0009, Datasheet §10).
// Vocabulary: Network Stream, Worker, Master Clock, Slot, Slot Table.
//
// Split:
//  - Worker (MMCSS) calls processTick() once per Master Clock tick: gathers OUT slots of type NETWORK into
//    Tx rings and scatters Rx rings into IN slots of type NETWORK. No allocation, no locks, no sockets.
//  - A network thread owns the UDP socket, PCM packing, Vorbis encode/decode, r8brain SRC and all allocation.
// Mapping: slot.type == SLOT_NETWORK, slot.streamId = Tx/Rx index 0..7, slot.srcChannel = channel in that stream.
// A Tx stream is live when netTx[i].ip is a valid IPv4 address and some OUT slot maps to it;
// an Rx stream is live when some IN slot maps to it (netRx[i].ip, if set, filters the sender).

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "WHASlotTable.h"

namespace wha {

struct WHANetworkCounters {
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> lost{0};       // Rx: sequence gaps
  std::atomic<uint64_t> dropped{0};    // Rx: late/duplicate, wrong peer, config mismatch, drift trim
  std::atomic<uint64_t> underruns{0};  // Rx: Worker found less than one tick buffered (re-primes)
  std::atomic<uint64_t> overflows{0};  // ring full (frames lost)
  std::atomic<uint64_t> errors{0};     // send failures, codec failures
};

struct WHANetworkOptions {
  uint16_t rxPort = 6980;   // UDP 6980 audio (tests use other ports)
  uint32_t ringMs = 250;    // per-stream ring capacity
};

class WHANetworkEngine {
 public:
  WHANetworkEngine(const WHASlotTable* table, WHANetworkOptions options = {});
  ~WHANetworkEngine();
  WHANetworkEngine(const WHANetworkEngine&) = delete;
  WHANetworkEngine& operator=(const WHANetworkEngine&) = delete;

  bool start();
  void stop();
  // After TableChanged: re-read netTx/netRx/slot mapping on the network thread.
  void requestReconfigure();

  // Worker side. masterOut/masterIn point at slot 0; slot n is at base + n * slotStride floats.
  void processTick(const float* masterOut, float* masterIn, size_t slotStride, uint32_t frames);

  const WHANetworkCounters& txCounters(uint32_t stream) const;
  const WHANetworkCounters& rxCounters(uint32_t stream) const;
  bool txActive(uint32_t stream) const;
  bool rxActive(uint32_t stream) const;
  bool rxPrimed(uint32_t stream) const;
  uint32_t rxTargetFrames(uint32_t stream) const;
  uint64_t unattributedMalformed() const { return malformed_.load(); }
  // Reconfigurations applied so far (lets callers wait for requestReconfigure()).
  uint64_t configGeneration() const { return generation_.load(); }
  std::string socketError() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::atomic<uint64_t> malformed_{0};
  std::atomic<uint64_t> generation_{0};
};

}  // namespace wha
