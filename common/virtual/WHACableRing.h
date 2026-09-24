#pragma once

// WHACableRing - one direction of a Virtual Cable: a stereo float FIFO between a Windows stream (the
// driver's WaveRT stream) and the Worker. Plain C++ without the C++ library, so WinHookAudio.sys and
// an offline test share it. Not thread-safe: the driver holds the cable's spin lock around each call.
// Vocabulary: Virtual Cable, Worker.

namespace wha {

class WHACableRing {
 public:
  static constexpr unsigned kFrames = 8192;  // 64 KB of stereo float

  void clear() {
    read_ = 0;
    fill_ = 0;
    primed_ = false;
  }
  unsigned fill() const { return fill_; }
  bool primed() const { return primed_; }
  unsigned underruns() const { return underruns_; }
  unsigned drops() const { return drops_; }

  // Appends n frames (src nullptr: silence). When full, the oldest frames are dropped.
  void write(const float* src, unsigned n) {
    if (n > kFrames) {
      if (src) src += 2 * (n - kFrames);
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
      data_[2 * w] = src ? src[2 * i] : 0.0f;
      data_[2 * w + 1] = src ? src[2 * i + 1] : 0.0f;
      w = w + 1 == kFrames ? 0 : w + 1;
    }
    fill_ += n;
  }

  // Takes n frames into dst. The reader starts once `prime` frames are queued, silence before: that
  // is the cable's latency, which absorbs both sides' block sizes and timing. A read that finds fewer
  // than n frames gives what there is, then silence, and waits for `prime` again (an underrun). More
  // than prime + slack frames queued (the two sides' clocks drift apart) drops the oldest down to
  // prime.
  void read(float* dst, unsigned n, unsigned prime, unsigned slack) {
    if (prime > kFrames) prime = kFrames;
    if (!primed_) {
      if (fill_ < prime || fill_ == 0) {
        zero(dst, n);
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
      dst[2 * i] = data_[2 * r];
      dst[2 * i + 1] = data_[2 * r + 1];
      r = r + 1 == kFrames ? 0 : r + 1;
    }
    read_ = r;
    fill_ -= take;
    if (take < n) {
      zero(dst + 2 * take, n - take);
      ++underruns_;
      primed_ = false;
    }
  }

 private:
  static void zero(float* dst, unsigned n) {
    for (unsigned i = 0; i < 2 * n; ++i) dst[i] = 0.0f;
  }

  float data_[2 * kFrames] = {};
  unsigned read_ = 0;  // frame index of the oldest queued frame
  unsigned fill_ = 0;  // frames queued
  bool primed_ = false;
  unsigned underruns_ = 0;
  unsigned drops_ = 0;
};

}  // namespace wha
