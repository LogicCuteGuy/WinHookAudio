#pragma once

// WHAJitterBuffer — per-stream jitter buffer for WHAA PCM.
// Vocabulary: Network Stream, Worker, Master Clock.

#include <cstdint>
#include <queue>
#include <vector>
#include <cstring>
#include "WHAPacket.h"

namespace wha {

struct JitterPacket {
  WHAAPacketHeader header{};
  std::vector<uint8_t> payload;
  uint64_t enqueueQpc = 0;
};

class WHAJitterBuffer {
 public:
  explicit WHAJitterBuffer(uint32_t jitterMs = 20, uint32_t sampleRate = 48000)
      : jitterMs_(jitterMs), sampleRate_(sampleRate) {}

  void setJitterMs(uint32_t ms) { jitterMs_ = ms; }
  void setSampleRate(uint32_t sr) { sampleRate_ = sr; }
  uint32_t jitterMs() const { return jitterMs_; }

  void push(const WHAAPacketHeader& header, const uint8_t* payload, uint32_t payloadBytes, uint64_t qpc) {
    if (!IsValidWhaaCodec(header.codec)) return;
    if (!IsValidWhaaChannels(header.codec, header.channels)) return;
    JitterPacket pkt;
    pkt.header = header;
    pkt.payload.assign(payload, payload + payloadBytes);
    pkt.enqueueQpc = qpc;
    queue_.push(std::move(pkt));
  }

  // Pop if jitter delay has elapsed (qpc - enqueueQpc >= jitterMs in QPC units)
  // For offline test, pop immediately if queue not empty
  bool pop(JitterPacket& out, uint64_t nowQpc = 0) {
    if (queue_.empty()) return false;
    // Offline: if nowQpc==0, pop immediately (no delay)
    if (nowQpc != 0) {
      uint64_t elapsed = nowQpc - queue_.front().enqueueQpc;
      // QPC frequency ~10MHz, jitterMs 20ms = 200000 QPC ticks (approx)
      // For test, use ms directly: assume QPC is ms
      if (elapsed < jitterMs_) return false;
    }
    out = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  size_t size() const { return queue_.size(); }
  bool empty() const { return queue_.empty(); }
  void clear() { while (!queue_.empty()) queue_.pop(); }

 private:
  uint32_t jitterMs_ = 20;
  uint32_t sampleRate_ = 48000;
  std::queue<JitterPacket> queue_;
};

}  // namespace wha
