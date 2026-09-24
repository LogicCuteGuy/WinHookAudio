#pragma once

// WHACableRing - one direction of a Virtual Cable: a float FIFO between a Windows stream (the driver's
// WaveRT stream) and the Worker. Every frame holds kCableChannels channels; each side writes and reads
// its own channel count (the rest are silent / dropped), so a stereo stream and a 7.1 Worker block
// share one ring. Plain C++ without the C++ library, so WinHookAudio.sys and an offline test share it.
// Not thread-safe: the driver holds the cable's spin lock around each call.
// Vocabulary: Virtual Cable, Worker.

#include "WHACableFormat.h"

namespace wha {

class WHACableRing {
 public:
  static constexpr unsigned kFrames = 8192;  // 256 KB: 8 channels of float

  void clear() {
    read_ = 0;
    fill_ = 0;
    primed_ = false;
  }
  unsigned fill() const { return fill_; }
  bool primed() const { return primed_; }
  unsigned underruns() const { return underruns_; }
  unsigned drops() const { return drops_; }

  // Appends n frames of `channels` interleaved channels (src nullptr: silence); channels past
  // `channels` are silent. When full, the oldest frames are dropped.
  void write(const float* src, unsigned n, unsigned channels) {
    if (channels > kCableChannels) channels = kCableChannels;
    if (n > kFrames) {
      if (src) src += channels * (n - kFrames);
      drops_ += n - kFrames;
      n = kFrames;
    }
    if (fill_ + n > kFrames) {
      const unsigned over = fill_ + n - kFrames;
      read_ = (read_ + over) % kFrames;
      fill_ -= over;
      drops_ += over;
    }
    unsigned w = (read_ + fill_) % kFrames;
    for (unsigned i = 0; i < n; ++i) {
      float* frame = data_ + kCableChannels * w;
      for (unsigned c = 0; c < kCableChannels; ++c) frame[c] = src && c < channels ? src[channels * i + c] : 0.0f;
      w = w + 1 == kFrames ? 0 : w + 1;
    }
    fill_ += n;
  }

  // Takes n frames of `channels` interleaved channels into dst (channels past kCableChannels are
  // silent). The reader starts once `prime` frames are queued, silence before: that is the cable's
  // latency, which absorbs both sides' block sizes and timing. A read that finds fewer than n frames
  // gives what there is, then silence, and waits for `prime` again (an underrun). More than
  // prime + slack frames queued (the two sides' clocks drift apart) drops the oldest down to prime.
  void read(float* dst, unsigned n, unsigned channels, unsigned prime, unsigned slack) {
    if (prime > kFrames) prime = kFrames;
    if (!primed_) {
      if (fill_ < prime || fill_ == 0) {
        zero(dst, n * channels);
        return;
      }
      primed_ = true;
    }
    if (fill_ > prime + slack) {
      const unsigned over = fill_ - prime;
      read_ = (read_ + over) % kFrames;
      fill_ -= over;
      drops_ += over;
    }
    const unsigned take = n < fill_ ? n : fill_;
    unsigned r = read_;
    for (unsigned i = 0; i < take; ++i) {
      const float* frame = data_ + kCableChannels * r;
      for (unsigned c = 0; c < channels; ++c) dst[channels * i + c] = c < kCableChannels ? frame[c] : 0.0f;
      r = r + 1 == kFrames ? 0 : r + 1;
    }
    read_ = r;
    fill_ -= take;
    if (take < n) {
      zero(dst + channels * take, channels * (n - take));
      ++underruns_;
      primed_ = false;
    }
  }

 private:
  static void zero(float* dst, unsigned samples) {
    for (unsigned i = 0; i < samples; ++i) dst[i] = 0.0f;
  }

  float data_[kCableChannels * kFrames] = {};
  unsigned read_ = 0;  // frame index of the oldest queued frame
  unsigned fill_ = 0;  // frames queued
  bool primed_ = false;
  unsigned underruns_ = 0;
  unsigned drops_ = 0;
};

}  // namespace wha
