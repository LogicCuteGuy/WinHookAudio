#pragma once

// WHAJitterBuffer — per-stream jitter buffer for WHAA PCM.
// Vocabulary: Network Stream, Worker, Master Clock.

#include <array>
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
    if (!IsValidWhaaPayload(payloadBytes)) return;
    if (header.streamId >= kNetStreams) return;
    JitterPacket pkt;
    pkt.header = header;
    pkt.payload.assign(payload, payload + payloadBytes);
    pkt.enqueueQpc = qpc;
    queues_[header.streamId].push(std::move(pkt));
  }

  bool pop(uint32_t streamId, JitterPacket& out, uint64_t nowQpc = 0, uint64_t qpcFreq = 0) {
    if (streamId >= kNetStreams) return false;
    auto& q = queues_[streamId];
    if (q.empty()) return false;
    if (nowQpc != 0 && qpcFreq != 0) {
      uint64_t elapsed = nowQpc - q.front().enqueueQpc;
      uint64_t jitterQpc = jitterMs_ * qpcFreq / 1000;
      if (elapsed < jitterQpc) return false;
    }
    out = std::move(q.front());
    q.pop();
    return true;
  }

  // Offline: pop immediately if queue not empty (no delay)
  bool pop(JitterPacket& out, uint64_t /*nowQpc*/ = 0) {
    for (uint32_t i = 0; i < kNetStreams; ++i) {
      if (!queues_[i].empty()) {
        out = std::move(queues_[i].front());
        queues_[i].pop();
        return true;
      }
    }
    return false;
  }

  size_t size() const {
    size_t n = 0;
    for (auto& q : queues_) n += q.size();
    return n;
  }
  size_t size(uint32_t streamId) const {
    if (streamId >= kNetStreams) return 0;
    return queues_[streamId].size();
  }
  bool empty() const { return size() == 0; }
  void clear() { for (auto& q : queues_) while (!q.empty()) q.pop(); }

 private:
  uint32_t jitterMs_ = 20;
  uint32_t sampleRate_ = 48000;
  std::array<std::queue<JitterPacket>, kNetStreams> queues_;
};

}  // namespace wha
