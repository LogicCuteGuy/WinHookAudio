#pragma once

// HwClockPacer — when the HW output paces the Master Clock, decide when the next tick is due so the
// ticks are evenly spaced at the device's own average rate. Vocabulary: Master Clock, Worker.
//
// Ticking whenever the device has room for a block (the first HW-paced clock) copies the device's
// drain pattern into the DAW: a device that plays its buffer in large chunks (VMware HD Audio drains
// ~1024 frames at once) got bursts of back-to-back bufferSwitch calls and then long gaps. The DAW had
// to compute a whole burst at once, and HW input reads came in bursts that emptied its backlog.
//
// The pacer watches the device's consumed position (frames written - padding) and runs a
// delay-locked loop over it: a smooth consumed position, advancing at the device's measured rate,
// that passes through the device's position reports on average. A tick is due when the smooth fill
// (written - smooth consumed) has come down to the setpoint. The device's reported fill then moves
// between the setpoint (just after it takes a chunk) and setpoint + chunk; the chunk (how far the
// device's reports jump beyond smooth consumption) is learned, and the setpoint is as high as the
// buffer allows (the most queued against our own stalls) while the top of that range plus one block
// still fits:
//   setpoint = capacity - block - max(chunk, block)   (at least one block)
// For a device that reports smoothly that is the first clock's threshold (capacity - 2 blocks), so its
// latency is unchanged. Hard limits stay: no room for a block -> wait; a fill below that range (the
// device prebuffering at start, our own stall) -> tick now until it is back.
// Warm-up: for its first second a device may take frames faster than it plays them (VMware HD Audio
// fills its own buffer at ~130% of real time), which no timeline at the device's rate can follow.
// Until then ticks follow the device's fill (the first clock's rule); the timeline then starts afresh
// at the next position report, so the start-up rush is not in its rate or phase.
// Windows mixer (a Shared device, ADR 0014): it takes one mixer period per wake and two after a late
// one. Learned, the chunk flipped between one and two periods as a double take entered and left its
// window, and each flip moved the setpoint, so the Master Clock's phase jumped against the device by
// half a thousand frames (HW inputs read it as drift, or starved). With the mixer period given, the
// chunk is two periods from the start and the fill runs setpoint..setpoint + one period.
// The mixer's wakes jitter by milliseconds, and now and then it misses one and stays a period behind
// for a quarter second before a double take catches up. A timeline at 1 Hz followed those lags,
// slowing the clock and then hurrying after the catch-up: its phase wandered by ~700 frames against the
// device (an exclusive HW input, dead band ~120 frames, read that as drift). So after the warm-up the
// timeline runs at 0.1 Hz there (reports come every period): ~150 frames. (Weighting late reports less,
// as the lags are late only, looked better offline but biased the rate by ~1000 ppm live.) The mixer
// has no start-up rush (one prebuffer take, then real time), so the timeline is not restarted after the
// warm-up: it keeps the phase it locked at 1 Hz and only its rate starts again at nominal (the 1 Hz rate
// estimate is ~1000 ppm off with the wake jitter; restarted, a look's phase error), which a 0.1 Hz loop
// would take seconds to work off while the HW inputs, just started, read it as drift. A real device's
// few hundred ppm it then learns with under a millisecond of phase error.
// No I/O: the clock thread feeds it WASAPI padding, tests a simulated device.

#include <cstdint>

namespace wha {

class HwClockPacer {
 public:
  // rate: nominal frames/s; block: frames per tick; capacity: device buffer, frames. mixerPeriod: the
  // Windows mixer's period when the device is Shared (0: an exclusive device, its chunk learned).
  void reset(double rate, int block, int capacity, int mixerPeriod = 0) {
    rate_ = rate;
    block_ = block;
    capacity_ = capacity;
    framePeriod_ = 1.0 / rate;
    have_ = false;
    anchored_ = false;
    warm_ = false;
    warmUntil_ = 0.0;
    written_ = consumedNewest_ = lastConsumed_ = 0;
    padding_ = 0;
    lastSec_ = newestSec_ = windowStart_ = 0.0;
    windowMax_ = previousMax_ = 0;
    mixerPeriod_ = mixerPeriod > 0 ? mixerPeriod : 0;
    chunk_ = roundChunk(2 * mixerPeriod_);
    hurries_ = 0;
  }

  // An observation at `nowSec`: frames written to the device so far, and its current padding.
  void observe(double nowSec, int64_t written, int32_t padding) {
    written_ = written;
    padding_ = padding;
    const int64_t consumed = written - padding;
    if (!have_) {
      have_ = true;
      lastConsumed_ = consumedNewest_ = consumed;
      lastSec_ = newestSec_ = windowStart_ = nowSec;
      return;
    }
    const double elapsed = nowSec - lastSec_;
    lastSec_ = nowSec;
    if (consumed <= lastConsumed_) return;
    const int64_t step = consumed - lastConsumed_;
    lastConsumed_ = consumed;
    if (mixerPeriod_ > 0 && anchored_ && !warm_ && nowSec >= warmUntil_) {  // no restart, see above
      warm_ = true;
      framePeriod_ = 1.0 / rate_;
    }
    if (!anchored_ || (!warm_ && nowSec >= warmUntil_)) {
      // The timeline starts at a position report, not at our first look: a device takes its first
      // chunk at once when started (it prebuffers), and a timeline begun earlier starts that much out
      // of phase. It starts once at the first report and again after the warm-up, at the nominal rate.
      if (!anchored_) warmUntil_ = nowSec + kWarmupSec;
      else warm_ = true;
      anchored_ = true;
      newestSec_ = nowSec;
      if (!warm_) windowStart_ = nowSec;
      framePeriod_ = 1.0 / rate_;
      consumedNewest_ = consumed;
      return;
    }
    // Chunk: how far a report jumps beyond what smooth play gave since the last look. Ours stalling
    // (a long elapsed) is not a chunk; the device playing a burst it had held back is.
    const double excess = static_cast<double>(step) - elapsed / framePeriod_;
    learnChunk(nowSec, excess > 0 ? static_cast<int32_t>(excess) : 0);
    track(step, nowSec);
    consumedNewest_ = consumed;
  }

  // Seconds from `nowSec` until the next tick is due (<= 0: tick now), from the last observation.
  double untilTick(double nowSec) {
    const int32_t room = capacity_ - padding_;
    if (room < block_) return (block_ - room) * framePeriod_ > 0.0002 ? (block_ - room) * framePeriod_ : 0.0002;
    if (!warm_) return (padding_ - warmFill()) * framePeriod_;  // warm-up: follow the fill
    // Below the range the smooth timeline keeps it in (setpoint..setpoint + chunk): the device took
    // more than its rate (it prebuffers at start, or we stalled). Catch up now, as the first clock did.
    if (padding_ < block_ / 2 || padding_ < setpoint() - block_) {
      ++hurries_;
      return 0.0;
    }
    return (smoothFill(nowSec) - setpoint()) * framePeriod_;
  }

  // Fill (frames) the smooth timeline ticks at; see the header comment.
  int32_t setpoint() const {
    const int32_t s = capacity_ - block_ - (chunk_ > block_ ? chunk_ : block_);
    return s > block_ ? s : block_;
  }
  // Mean device fill when a tick is due: the reported fill runs from the setpoint to setpoint + chunk
  // (the Windows mixer: + one period, its usual take).
  int32_t expectedFill() const { return setpoint() + (mixerPeriod_ > 0 ? mixerPeriod_ : chunk_) / 2; }
  // Fill the warm-up ticks at: the first clock's threshold, or through the Windows mixer the mean fill
  // the timeline will hold, so the Master Clock does not pause when the timeline takes over.
  int32_t warmFill() const { return mixerPeriod_ > 0 ? expectedFill() : capacity_ - 2 * block_; }
  int32_t chunk() const { return chunk_; }
  bool warm() const { return warm_; }  // warm-up over: ticks follow the smooth timeline
  uint64_t hurries() const { return hurries_; }
  double rate() const { return 1.0 / framePeriod_; }  // device frames/s, measured

 private:
  // Delay-locked loop (as HwInputFifo::track): newestSec_ = smoothed time the device reached
  // consumedNewest_ + step, framePeriod_ = device seconds per frame.
  void track(int64_t step, double nowSec) {
    const double interval = static_cast<double>(step) * framePeriod_;
    const double w = 2.0 * 3.14159265358979323846 * (mixerPeriod_ > 0 && warm_ ? kMixerDllHz : kDllHz) * interval;
    const double predicted = newestSec_ + interval;
    const double e = nowSec - predicted;
    newestSec_ = predicted + 1.41421356237 * w * e;
    framePeriod_ += w * w * e / static_cast<double>(step);
    // A wild estimate (a device that stopped, a clock jump) must not steer the Master Clock off rate.
    const double nominal = 1.0 / rate_;
    if (framePeriod_ < nominal * 0.95 || framePeriod_ > nominal * 1.05) framePeriod_ = nominal;
  }
  double smoothFill(double nowSec) const {
    if (!anchored_) return static_cast<double>(padding_);  // no timeline yet: the device's own fill
    double consumed = static_cast<double>(consumedNewest_) + (nowSec - newestSec_) / framePeriod_;
    if (consumed > static_cast<double>(written_)) consumed = static_cast<double>(written_);
    return static_cast<double>(written_) - consumed;
  }
  // chunk_ = the largest excess over the last one to two seconds (two one-second windows), so a
  // device that stops playing in chunks gives its latency back.
  void learnChunk(double nowSec, int32_t excess) {
    if (nowSec - windowStart_ >= 1.0) {
      previousMax_ = windowMax_;
      windowMax_ = 0;
      windowStart_ = nowSec;
    }
    if (excess > windowMax_) windowMax_ = excess;
    const int32_t c = windowMax_ > previousMax_ ? windowMax_ : previousMax_;
    chunk_ = roundChunk(c > 2 * mixerPeriod_ ? c : 2 * mixerPeriod_);
  }
  // Round up to a block so the setpoint (and the reported latency) does not move on noise.
  int32_t roundChunk(int32_t c) const { return c < block_ / 2 ? 0 : ((c + block_ - 1) / block_) * block_; }

  static constexpr double kDllHz = 1.0;
  static constexpr double kMixerDllHz = 0.1;
  static constexpr double kWarmupSec = 1.0;
  double rate_ = 48000.0;
  int32_t block_ = 128;
  int32_t capacity_ = 512;
  double framePeriod_ = 1.0 / 48000.0;
  bool have_ = false;
  bool anchored_ = false;  // the device has moved: the timeline has a starting point
  bool warm_ = false;      // warm-up over, timeline restarted
  double warmUntil_ = 0.0;
  int64_t written_ = 0;
  int32_t padding_ = 0;
  int64_t consumedNewest_ = 0;
  int64_t lastConsumed_ = 0;
  double lastSec_ = 0.0;
  double newestSec_ = 0.0;
  double windowStart_ = 0.0;
  int32_t windowMax_ = 0;
  int32_t previousMax_ = 0;
  int32_t chunk_ = 0;
  int32_t mixerPeriod_ = 0;
  uint64_t hurries_ = 0;
};

}  // namespace wha
