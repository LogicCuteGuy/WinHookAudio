#pragma once

// HwInputFifo — HW input frames from a device on its own clock, read one block per Master Clock
// tick. Vocabulary: Master Clock, Worker, Slot.
//
// push(): device packets (interleaved) -> drift resampler -> FIFO. read(): one planar block per
// tick. The controlled quantity is the backlog: frames queued plus frames the device has produced but
// not yet delivered. A delay-locked loop over (frames delivered, packet time) learns the device's
// frame period and a smooth time for its newest delivered frame, so the backlog has no sawtooth from
// packet delivery (which aliases against the reads into slow swings) and no delivery jitter,
// whichever the packet times are (capture or delivery stamps; devices differ), and is constant for
// equal clocks.
// Reads start with the backlog at exactly the priming target; starve -> silence, the target grows by
// a quarter (at least one device period: delivery jitter it did not cover; capped) and reads
// re-prime; overflow or a
// queue far above target -> trim. A PI loop (WHADriftControl) holds the backlog at the target by
// resampling, so a device clock that differs from the Master Clock never starves or trims. Equal
// clocks never engage it and the path stays a bit-exact copy.
// No I/O, no COM: KsCapture feeds it from WASAPI, tests from a simulated device.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "WHADriftControl.h"
#include "network/WHADriftResampler.h"

namespace wha {

class HwInputFifo {
 public:
  // period: device packet size, frames (packets arrive a period at a time).
  void reset(double rate, int channels, int block, int period) {
    rate_ = rate;
    channels_ = channels;
    block_ = block;
    period_ = period;
    prime_ = 2 * block + period;
    maxPrime_ = prime_ + 16 * period > static_cast<int>(rate / 10) ? prime_ + 16 * period : static_cast<int>(rate / 10);  // >= 100 ms
    capacity_ = static_cast<size_t>(maxPrime_ + period + 16 * block);
    resampled_.reserve(static_cast<size_t>(4 * (period + block)) * channels);
    fifo_.assign(capacity_ * channels_, 0.0f);
    readPos_ = count_ = 0;
    primed_ = false;
    haveTime_ = false;
    resampler_.reset(static_cast<uint32_t>(channels));
    resetDrift();
    reads_ = starved_ = trims_ = growths_ = skipped_ = 0;
    latencyChanged_ = false;
    fill_ = 0;
    fillSum_ = 0;
    ppmMilli_ = 0;
    engaged_ = false;
  }

  // Called from the Worker (or a test) with interleaved frames; packetSec: the packet's time stamp
  // (capture or delivery), on the same clock as read()'s nowSec.
  void push(const float* interleaved, uint32_t frames, double packetSec) {
    track(frames, packetSec);
    resampled_.clear();
    resampler_.process(interleaved, frames, drift_.ratio(), resampled_);
    const size_t out = resampled_.size() / static_cast<size_t>(channels_);
    bool overflowed = false;
    for (size_t f = 0; f < out; ++f) {
      if (count_ == capacity_) {  // full: the oldest frame goes
        readPos_ = (readPos_ + 1) % capacity_;
        --count_;
        overflowed = true;
      }
      std::memcpy(&fifo_[((readPos_ + count_) % capacity_) * channels_], &resampled_[f * channels_], sizeof(float) * channels_);
      ++count_;
    }
    if (overflowed) trims_.fetch_add(1);
  }

  // One Master Clock tick at nowSec: `frames` of planar `data` (channel c at data[c * frames]).
  // Returns true if real audio (false: silence while priming or starved).
  bool read(float* data, int frames, int channels, double nowSec) {
    std::memset(data, 0, sizeof(float) * static_cast<size_t>(frames) * static_cast<size_t>(channels));
    const size_t need = static_cast<size_t>(frames);
    const size_t top = static_cast<size_t>(prime_) + need;
    if (!primed_ && backlog(nowSec) >= static_cast<double>(top) && count_ >= need) {
      // Start with the backlog exactly at the target (not wherever the last packet left the queue):
      // the same input latency every run.
      const size_t excess = static_cast<size_t>(backlog(nowSec) - static_cast<double>(top));
      drop(excess < count_ - need ? excess : count_ - need);
      primed_ = true;
    }
    if (primed_ && count_ > static_cast<size_t>(prime_ + period_) + 8 * need) {  // far above target: resync
      drop(count_ - (static_cast<size_t>(prime_) + need));
      trims_.fetch_add(1);
    }
    const double measured = backlog(nowSec);
    fill_ = static_cast<int32_t>(measured);
    if (!primed_) return false;
    if (count_ < need) {  // behind the Master Clock: silence, a larger target, then re-prime
      starved_.fetch_add(1);
      primed_ = false;
      const int grow = prime_ / 4 > period_ ? prime_ / 4 : period_;
      if (prime_ < maxPrime_) {
        prime_ = prime_ + grow < maxPrime_ ? prime_ + grow : maxPrime_;
        growths_.fetch_add(1);
        latencyChanged_ = true;
        drift_.retarget(expectedFill(), deadband());
      }
      return false;
    }
    // One read per tick = frames / rate seconds.
    drift_.update(measured, static_cast<double>(frames) / rate_);
    ppmMilli_ = static_cast<int32_t>(drift_.sourcePpm() * 1000.0);
    engaged_ = drift_.engaged();
    for (size_t f = 0; f < need; ++f) {
      const float* src = &fifo_[((readPos_ + f) % capacity_) * channels_];
      for (int c = 0; c < channels && c < channels_; ++c) data[static_cast<size_t>(c) * need + f] = src[c];
    }
    fillSum_.fetch_add(static_cast<uint64_t>(measured));
    readPos_ = (readPos_ + need) % capacity_;
    count_ -= need;
    reads_.fetch_add(1);
    return true;
  }

  // The Master Clock ticked `frames` worth of blocks without a read (the Worker missed ticks, and
  // those DAW inputs were served stale): drop the same frames, so reads stay one per tick and the
  // backlog stays on target instead of being mistaken for drift.
  void skip(size_t frames) {
    if (!primed_) return;
    drop(frames < count_ ? frames : count_);
    skipped_.fetch_add(frames);
  }

  int32_t targetFill() const { return prime_; }
  // Backlog held at a read (before the block is taken): the age, in frames, of the frame being read.
  int32_t expectedFill() const { return prime_ + block_; }
  // Frames the resampler holds before the FIFO (constant; bit-exact delay at ratio 1).
  static constexpr int32_t resamplerDelay() { return WHADriftResampler::kTaps - (WHADriftResampler::kHalf - 1); }

  // Set when the target grew (input latency changed); the driver tells the DAW and clears it.
  bool takeLatencyChanged() { return latencyChanged_.exchange(false); }

  // Counters (any thread). fill()/meanFill(): backlog at a read.
  uint64_t growths() const { return growths_.load(); }
  uint64_t skipped() const { return skipped_.load(); }  // frames dropped for missed ticks
  uint64_t reads() const { return reads_.load(); }
  uint64_t starved() const { return starved_.load(); }
  uint64_t trims() const { return trims_.load(); }
  int32_t fill() const { return fill_.load(); }
  int32_t meanFill() const {
    const uint64_t n = reads_.load();
    return n ? static_cast<int32_t>(fillSum_.load() / n) : -1;
  }
  double sourcePpm() const { return ppmMilli_.load() / 1000.0; }
  double ratio() const { return drift_.ratio(); }  // Worker thread / tests
  bool driftEngaged() const { return engaged_.load(); }

 private:
  // Delay-locked loop (2nd order, kDllHz bandwidth): newestSec_ = smoothed time of the newest
  // delivered frame, framePeriod_ = device seconds per frame.
  void track(uint32_t frames, double packetSec) {
    if (!haveTime_ || frames == 0) {
      if (frames) { newestSec_ = packetSec; framePeriod_ = 1.0 / rate_; haveTime_ = true; }
      return;
    }
    const double interval = frames * framePeriod_;
    const double w = 2.0 * 3.14159265358979323846 * kDllHz * interval;
    const double predicted = newestSec_ + interval;
    const double e = packetSec - predicted;
    newestSec_ = predicted + 1.41421356237 * w * e;
    framePeriod_ += w * w * e / frames;
  }
  double backlog(double nowSec) const {
    const double undelivered = haveTime_ && nowSec > newestSec_ ? (nowSec - newestSec_) / framePeriod_ : 0.0;
    return static_cast<double>(count_) + undelivered;
  }
  double deadband() const { return prime_ / 5.0 > 96.0 ? prime_ / 5.0 : 96.0; }
  void resetDrift() {
    WHADriftParams params;
    params.deadband = deadband();
    drift_.reset(expectedFill(), rate_, params);
  }
  void drop(size_t frames) {
    readPos_ = (readPos_ + frames) % capacity_;
    count_ -= frames;
  }

  double rate_ = 48000.0;
  int channels_ = 2;
  int block_ = 128;
  int period_ = 128;
  int prime_ = 0;
  int maxPrime_ = 0;
  size_t capacity_ = 0;
  std::vector<float> fifo_;
  std::vector<float> resampled_;
  size_t readPos_ = 0;
  size_t count_ = 0;
  bool primed_ = false;
  static constexpr double kDllHz = 1.0;
  double newestSec_ = 0.0;
  double framePeriod_ = 1.0 / 48000.0;
  bool haveTime_ = false;
  WHADriftResampler resampler_;
  WHADriftControl drift_;

  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> starved_{0};
  std::atomic<uint64_t> trims_{0};
  std::atomic<int32_t> fill_{0};
  std::atomic<uint64_t> fillSum_{0};
  std::atomic<int32_t> ppmMilli_{0};
  std::atomic<bool> engaged_{false};
  std::atomic<uint64_t> growths_{0};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<bool> latencyChanged_{false};
};

}  // namespace wha
