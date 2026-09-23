#pragma once

// HwOutputFifo — Master Clock blocks to a HW output on its own clock: an output device other than the
// Master Clock's. Vocabulary: Master Clock, Worker, Slot.
//
// Once per Master Clock tick: push() takes the block (planar) through a drift resampler into a queue;
// plan() looks at the device (frames written so far, its padding) and says how many frames to write
// now; pop() hands them out. The controlled quantity is the backlog: frames queued here plus the
// device's fill. A PI loop (WHADriftControl) resamples to hold it at the target, so a device clock
// that differs from the Master Clock never runs dry or overflows. Equal clocks never engage it and
// the path stays a bit-exact copy.
//
// The device's fill comes from a smooth timeline of its consumed position (HwPositionTracker below),
// not from its padding: many devices report their position in steps (VMware HD Audio: 512 frames),
// and we only look once per tick, so the padding is a sawtooth whose phase against the ticks drifts
// slowly with the clock difference. A loop that followed it wobbled the pitch by thousands of ppm.
//
// Target backlog after a push: capacity - max(chunk, block), so after the write the device holds as
// much as fits with room for its reporting steps, as the Master Clock's own device does.
// Hard limits: a backlog below half the target for 25 ms -> silence up to the target (a gap, counted;
// a device that ran dry for a moment because the Worker stalled is refilled by the catch-up ticks
// instead); a backlog far above target -> the oldest frames go (counted). Until the timeline is warm
// (a second after the device first moves: VMware HD Audio takes frames faster than real time at
// start) the drift loop waits.
// No I/O, no COM: MasterHolder feeds it from WASAPI, tests from a simulated device.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "WHADriftControl.h"
#include "network/WHADriftResampler.h"

namespace wha {

// A smooth consumed position for a device observed at arbitrary times that may report its position in
// steps. A report c at time t says the device's position (up to a constant offset) is in [c, c + g),
// g = the step (0 for a smooth device). A second-order loop corrects the timeline only by how far it
// lies outside that interval, so a timeline running at the device's rate is never pulled around by
// where in a step we happened to look. g is learned: how far a report jumps beyond what the device
// could have played since the previous look (as HwClockPacer's chunk), plus the look interval.
class HwPositionTracker {
 public:
  void reset(double rate, int block) {
    nominal_ = rate;
    rate_ = rate;
    block_ = block;
    have_ = anchored_ = warm_ = false;
    lastConsumed_ = 0;
    lastSec_ = sec_ = pos_ = warmUntil_ = windowStart_ = 0.0;
    windowMax_ = previousMax_ = chunk_ = 0;
  }

  void observe(double nowSec, int64_t consumed) {
    if (!have_) {
      have_ = true;
      lastConsumed_ = consumed;
      lastSec_ = windowStart_ = nowSec;
      return;
    }
    const double elapsed = nowSec - lastSec_;
    const int64_t step = consumed - lastConsumed_;
    lastSec_ = nowSec;
    lastConsumed_ = consumed;
    if (step > 0 && anchored_) {
      const double excess = static_cast<double>(step) - elapsed * rate_;
      learnChunk(nowSec, excess > 0 ? static_cast<int32_t>(excess) : 0);
    }
    if (!anchored_ || (!warm_ && nowSec >= warmUntil_)) {
      // Start (and restart after the warm-up) at a report, at the nominal rate: a device takes its
      // first frames at once when started, and may take them faster than real time for a while.
      if (step <= 0) return;
      if (!anchored_) warmUntil_ = nowSec + kWarmupSec;
      else warm_ = true;
      anchored_ = true;
      sec_ = nowSec;
      pos_ = static_cast<double>(consumed);
      rate_ = nominal_;
      return;
    }
    if (elapsed <= 0.0) return;
    const double predicted = pos_ + (nowSec - sec_) * rate_;
    const double g = chunk_ > 0 ? static_cast<double>(chunk_ + block_) : 0.0;
    const double lo = static_cast<double>(consumed), hi = lo + g;
    const double e = predicted < lo ? lo - predicted : (predicted > hi ? hi - predicted : 0.0);
    const double w = 2.0 * 3.14159265358979323846 * kHz * elapsed;
    sec_ = nowSec;
    pos_ = predicted + 1.41421356237 * w * e;
    rate_ += w * w * e / elapsed;
    if (rate_ < nominal_ * 0.95 || rate_ > nominal_ * 1.05) rate_ = nominal_;  // a device that stopped
  }

  bool anchored() const { return anchored_; }
  bool warm() const { return warm_; }
  double consumedAt(double nowSec) const { return pos_ + (nowSec - sec_) * rate_; }
  int32_t chunk() const { return chunk_; }
  double rate() const { return rate_; }

 private:
  // chunk_ = the largest excess over the last one to two seconds, rounded up to a block.
  void learnChunk(double nowSec, int32_t excess) {
    if (nowSec - windowStart_ >= 1.0) {
      previousMax_ = windowMax_;
      windowMax_ = 0;
      windowStart_ = nowSec;
    }
    if (excess > windowMax_) windowMax_ = excess;
    const int32_t c = windowMax_ > previousMax_ ? windowMax_ : previousMax_;
    chunk_ = c < block_ / 2 ? 0 : ((c + block_ - 1) / block_) * block_;
  }

  static constexpr double kHz = 1.0;
  static constexpr double kWarmupSec = 1.0;
  double nominal_ = 48000.0;
  double rate_ = 48000.0;  // device frames per second
  int block_ = 128;
  bool have_ = false, anchored_ = false, warm_ = false;
  int64_t lastConsumed_ = 0;
  double lastSec_ = 0.0;
  double sec_ = 0.0, pos_ = 0.0;  // the timeline passes through pos_ at sec_
  double warmUntil_ = 0.0;
  double windowStart_ = 0.0;
  int32_t windowMax_ = 0, previousMax_ = 0, chunk_ = 0;
};

class HwOutputFifo {
 public:
  // capacity: the device buffer, frames. maxBlock: the largest push().
  void reset(double rate, int channels, int block, int capacity, int maxBlock = 4096) {
    rate_ = rate;
    channels_ = channels;
    block_ = block;
    capacity_ = capacity;
    position_.reset(rate, block);
    queueCap_ = static_cast<size_t>(capacity + 16 * block);
    queue_.assign(queueCap_ * channels_, 0.0f);
    interleaved_.assign(static_cast<size_t>(maxBlock) * channels_, 0.0f);
    resampled_.clear();
    resampled_.reserve(static_cast<size_t>(2 * maxBlock + 64) * channels_);
    readPos_ = count_ = 0;
    silenceDue_ = 0;
    lastBlock_ = block;
    lowSince_ = -1.0;
    gains_ = 0;
    engagedSince_ = -1.0;
    target_ = targetFor(0);
    resampler_.reset(static_cast<uint32_t>(channels));
    WHADriftParams params;
    params.deadband = deadband();
    drift_.reset(target_.load(), rate_, params);
    blocks_ = underruns_ = gaps_ = trims_ = 0;
    wrote_ = false;
    fill_ = 0;
    ppmMilli_ = 0;
    engaged_ = false;
    chunk_ = 0;
  }

  // Worker, once per tick: `frames` of planar `data` (channel c at data[c * frames]).
  void push(const float* data, int frames, int channels) {
    if (frames <= 0) return;
    if (static_cast<size_t>(frames) * channels_ > interleaved_.size()) frames = static_cast<int>(interleaved_.size() / channels_);
    for (int f = 0; f < frames; ++f)
      for (int c = 0; c < channels_; ++c)
        interleaved_[static_cast<size_t>(f) * channels_ + c] = c < channels ? data[static_cast<size_t>(c) * frames + f] : 0.0f;
    resampled_.clear();
    resampler_.process(interleaved_.data(), static_cast<uint32_t>(frames), drift_.ratio(), resampled_);
    const size_t out = resampled_.size() / static_cast<size_t>(channels_);
    size_t overflow = 0;
    for (size_t f = 0; f < out; ++f) {
      if (count_ == queueCap_) {  // full: the oldest frame goes
        readPos_ = (readPos_ + 1) % queueCap_;
        --count_;
        ++overflow;
      }
      std::memcpy(&queue_[((readPos_ + count_) % queueCap_) * channels_], &resampled_[f * channels_], sizeof(float) * channels_);
      ++count_;
    }
    if (overflow) trims_.fetch_add(1);
    lastBlock_ = frames;
    blocks_.fetch_add(1);
  }

  // The Master Clock ticked `frames` without a push (the Worker missed ticks; those blocks are lost):
  // silence in their place keeps the backlog on target instead of looking like drift.
  void skip(int frames) {
    if (frames > 0) silenceDue_ += static_cast<size_t>(frames);
  }

  // Worker, after push(): the device at `nowSec` (frames written to it so far, its padding). Returns
  // the frames to write now (at most its room); pop() them.
  int plan(double nowSec, int64_t written, int32_t padding) {
    position_.observe(nowSec, written - padding);
    chunk_ = position_.chunk();
    const int32_t t = targetFor(chunk_);
    if (t != target_.load()) {  // the device's steps changed: a new setpoint, keeping the drift estimate
      target_ = t;
      drift_.retarget(t, deadband());
    }
    // Loop time scale: 1 s for the first seconds of drift for a smooth device (it learns a large
    // drift before the backlog runs far), 4 s after that and from the start for one that reports in
    // steps (known only to within a step, and that uncertainty moves as the clocks slip against each
    // other), then 16 s for every device: the blocks do not arrive at an even rate either (a Master
    // Clock on a device that plays in chunks wanders by thousands of ppm over seconds), and the
    // backlog absorbs that while the pitch follows only the average.
    const double engagedFor = engagedSince_ >= 0.0 ? nowSec - engagedSince_ : 0.0;
    const int gains = engagedFor >= kLearnSec ? 2 : (chunk_.load() > 0 || engagedFor >= kFastSec ? 1 : 0);
    if (gains != gains_) {
      gains_ = gains;
      drift_.setGains(kKp[gains], kKi[gains]);
    }
    const bool warm = position_.warm();
    double fill = static_cast<double>(padding);
    if (warm) {
      fill = static_cast<double>(written) - position_.consumedAt(nowSec);
      if (fill > static_cast<double>(padding)) fill = padding;  // it cannot hold more than it says
    }
    if (fill < 0) fill = 0;
    double backlog = static_cast<double>(count_ + silenceDue_) + fill;
    if (padding == 0 && wrote_) underruns_.fetch_add(1);  // it ran dry: it played a gap
    const double low = target_.load() / 2.0;
    if (backlog < low) {
      if (lowSince_ < 0) lowSince_ = nowSec;
      if (nowSec - lowSince_ >= kLowSec) {  // still behind after the Worker could catch up: refill
        const double add = target_.load() - backlog;
        silenceDue_ += static_cast<size_t>(add);
        backlog += static_cast<double>(static_cast<size_t>(add));
        gaps_.fetch_add(1);
        lowSince_ = -1.0;
      }
    } else {
      lowSince_ = -1.0;
    }
    if (backlog > target_.load() + chunk_.load() + 4.0 * block_) {
      // Far ahead (the device stalled, or took less than it said): the oldest frames go.
      size_t drop = static_cast<size_t>(backlog - target_.load());
      const size_t fromSilence = drop < silenceDue_ ? drop : silenceDue_;
      silenceDue_ -= fromSilence;
      drop -= fromSilence;
      if (drop > count_) drop = count_;
      readPos_ = (readPos_ + drop) % queueCap_;
      count_ -= drop;
      backlog -= static_cast<double>(fromSilence + drop);
      trims_.fetch_add(1);
    }
    if (warm) {
      drift_.update(backlog, static_cast<double>(lastBlock_) / rate_);
      ppmMilli_ = static_cast<int32_t>(-drift_.sourcePpm() * 1000.0);  // the source here is the Master Clock
      engaged_ = drift_.engaged();
      if (engaged_ && engagedSince_ < 0.0) engagedSince_ = nowSec;
    }
    fill_ = static_cast<int32_t>(backlog);
    const size_t room = padding < capacity_ ? static_cast<size_t>(capacity_ - padding) : 0;
    const size_t have = count_ + silenceDue_;
    return static_cast<int>(have < room ? have : room);
  }

  // `frames` interleaved frames (at most what plan() returned): owed silence first, then the queue.
  void pop(float* out, int frames) {
    const size_t n = static_cast<size_t>(frames);
    const size_t silence = n < silenceDue_ ? n : silenceDue_;
    std::memset(out, 0, sizeof(float) * silence * channels_);
    silenceDue_ -= silence;
    size_t f = silence;
    for (; f < n && count_ > 0; ++f) {
      std::memcpy(&out[f * channels_], &queue_[readPos_ * channels_], sizeof(float) * channels_);
      readPos_ = (readPos_ + 1) % queueCap_;
      --count_;
    }
    if (f < n) std::memset(&out[f * channels_], 0, sizeof(float) * (n - f) * channels_);
    if (n) wrote_ = true;
  }

  // Backlog after a push, frames: what a frame pushed now waits behind before it plays.
  int32_t target() const { return target_.load(); }
  // Frames the resampler holds before the queue (constant; bit-exact delay at ratio 1).
  static constexpr int32_t resamplerDelay() { return WHADriftResampler::kTaps - (WHADriftResampler::kHalf - 1); }

  // Counters (any thread).
  uint64_t blocks() const { return blocks_.load(); }
  uint64_t underruns() const { return underruns_.load(); }  // device found empty: it played a gap
  uint64_t gaps() const { return gaps_.load(); }            // silence written to get back to target
  uint64_t trims() const { return trims_.load(); }          // times frames were thrown away
  int32_t fill() const { return fill_.load(); }             // backlog at the last tick
  int32_t chunk() const { return chunk_.load(); }           // the device's reporting step (0 = smooth)
  double devicePpm() const { return ppmMilli_.load() / 1000.0; }  // device clock vs Master Clock, + = device fast
  bool driftEngaged() const { return engaged_.load(); }
  double ratio() const { return drift_.ratio(); }  // Worker thread / tests

 private:
  int32_t targetFor(int32_t chunk) const {
    const int32_t t = capacity_ - (chunk > block_ ? chunk : block_);
    return t > 2 * block_ ? t : 2 * block_;
  }
  double deadband() const {
    double d = target_.load() / 5.0 > 96.0 ? target_.load() / 5.0 : 96.0;
    const double step = chunk_.load() > 0 ? (chunk_.load() + block_) / 2.0 : 0.0;
    return d > step ? d : step;
  }

  static constexpr double kLowSec = 0.025;
  // Critically damped PI gains for a T-second time scale: kp = 2 / T, ki = 1 / T^2 (T = 1, 4, 16).
  static constexpr double kKp[3] = {2.0, 0.5, 0.125};
  static constexpr double kKi[3] = {1.0, 1.0 / 16.0, 1.0 / 256.0};
  static constexpr double kFastSec = 5.0;
  static constexpr double kLearnSec = 15.0;
  double rate_ = 48000.0;
  int channels_ = 2;
  int block_ = 128;
  int32_t capacity_ = 512;
  HwPositionTracker position_;
  std::vector<float> queue_;
  std::vector<float> interleaved_;
  std::vector<float> resampled_;
  size_t queueCap_ = 0;
  size_t readPos_ = 0;
  size_t count_ = 0;
  size_t silenceDue_ = 0;
  int lastBlock_ = 128;
  double lowSince_ = -1.0;
  int gains_ = 0;              // index into kKp / kKi in use
  double engagedSince_ = -1.0;  // when the drift loop engaged
  bool wrote_ = false;
  WHADriftResampler resampler_;
  WHADriftControl drift_;

  std::atomic<int32_t> target_{0};  // read by stats (any thread)
  std::atomic<uint64_t> blocks_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> gaps_{0};
  std::atomic<uint64_t> trims_{0};
  std::atomic<int32_t> fill_{0};
  std::atomic<int32_t> ppmMilli_{0};
  std::atomic<bool> engaged_{false};
  std::atomic<int32_t> chunk_{0};
};

}  // namespace wha
