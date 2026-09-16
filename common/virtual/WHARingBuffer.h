#pragma once

// WHARingBuffer — 64KB RingBuffer per Virtual Cable (8× Stereo).
// Vocabulary: Virtual Cable, Worker.

#include <cstdint>
#include <array>
#include <cstring>
#include "WHAIoctl.h"

namespace wha {

class WHARingBuffer {
 public:
  WHARingBuffer() = default;

  bool write(const float* data, uint32_t frames) {
    if (frames == 0 || frames > kRingBufferFrames) return false;
    uint32_t available = kRingBufferFrames - size_;
    if (frames > available) return false;  // overflow
    for (uint32_t f = 0; f < frames; ++f) {
      for (uint32_t ch = 0; ch < kVirtualChannels; ++ch) {
        buffer_[(writePos_ + f) * kVirtualChannels + ch] = data[f * kVirtualChannels + ch];
      }
    }
    writePos_ = (writePos_ + frames) % kRingBufferFrames;
    size_ += frames;
    return true;
  }

  bool read(float* out, uint32_t frames) {
    if (frames == 0 || frames > kRingBufferFrames) return false;
    if (frames > size_) return false;  // underflow
    for (uint32_t f = 0; f < frames; ++f) {
      for (uint32_t ch = 0; ch < kVirtualChannels; ++ch) {
        out[f * kVirtualChannels + ch] = buffer_[(readPos_ + f) * kVirtualChannels + ch];
      }
    }
    readPos_ = (readPos_ + frames) % kRingBufferFrames;
    size_ -= frames;
    return true;
  }

  uint32_t size() const { return size_; }
  uint32_t available() const { return kRingBufferFrames - size_; }
  bool empty() const { return size_ == 0; }
  void clear() { readPos_ = 0; writePos_ = 0; size_ = 0; }

 private:
  std::array<float, kRingBufferFrames * kVirtualChannels> buffer_{};
  uint32_t readPos_ = 0;
  uint32_t writePos_ = 0;
  uint32_t size_ = 0;
};

}  // namespace wha
