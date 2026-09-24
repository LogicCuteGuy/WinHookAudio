#pragma once

// WHASpscRing — lock-free single-producer/single-consumer ring of interleaved float frames.
// Vocabulary: Network Stream, Worker.
// Sits between the MMCSS Worker (no allocation, no locks) and the network thread.
// allocate()/reset() must only run while neither side is using the ring.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wha {

class WHASpscRing {
 public:
  // capacityFrames is rounded up to a power of two.
  bool allocate(uint32_t channels, uint32_t capacityFrames) {
    if (channels == 0 || capacityFrames == 0) return false;
    uint32_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    buf_.assign(static_cast<size_t>(cap) * channels, 0.0f);
    channels_ = channels;
    capacity_ = cap;
    reset();
    return true;
  }
  void reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  uint32_t channels() const { return channels_; }
  uint32_t capacity() const { return capacity_; }
  uint32_t readable() const {
    return static_cast<uint32_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
  }
  uint32_t writable() const { return capacity_ - readable(); }

  // Producer side. Returns frames written (short when full).
  uint32_t write(const float* interleaved, uint32_t frames) {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint32_t space = capacity_ - static_cast<uint32_t>(head - tail_.load(std::memory_order_acquire));
    const uint32_t n = frames < space ? frames : space;
    copyIn(static_cast<uint32_t>(head & (capacity_ - 1)), interleaved, n);
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  // Consumer side. Returns frames read (short when empty).
  uint32_t read(float* interleaved, uint32_t frames) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t avail = static_cast<uint32_t>(head_.load(std::memory_order_acquire) - tail);
    const uint32_t n = frames < avail ? frames : avail;
    copyOut(static_cast<uint32_t>(tail & (capacity_ - 1)), interleaved, n);
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

  // Consumer side: drop frames without copying (drift trim).
  uint32_t discard(uint32_t frames) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t avail = static_cast<uint32_t>(head_.load(std::memory_order_acquire) - tail);
    const uint32_t n = frames < avail ? frames : avail;
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

 private:
  void copyIn(uint32_t at, const float* src, uint32_t frames) {
    const uint32_t first = frames < capacity_ - at ? frames : capacity_ - at;
    std::memcpy(&buf_[static_cast<size_t>(at) * channels_], src, sizeof(float) * first * channels_);
    if (frames > first)
      std::memcpy(buf_.data(), src + static_cast<size_t>(first) * channels_, sizeof(float) * (frames - first) * channels_);
  }
  void copyOut(uint32_t at, float* dst, uint32_t frames) const {
    const uint32_t first = frames < capacity_ - at ? frames : capacity_ - at;
    std::memcpy(dst, &buf_[static_cast<size_t>(at) * channels_], sizeof(float) * first * channels_);
    if (frames > first)
      std::memcpy(dst + static_cast<size_t>(first) * channels_, buf_.data(), sizeof(float) * (frames - first) * channels_);
  }

  std::vector<float> buf_;
  uint32_t channels_ = 0;
  uint32_t capacity_ = 0;
  std::atomic<uint64_t> head_{0};  // frames ever written
  std::atomic<uint64_t> tail_{0};  // frames ever read
};

}  // namespace wha
