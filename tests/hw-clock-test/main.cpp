// hw-clock-test — HwClockPacer against a simulated HW output device, offline and deterministic.
// The device plays continuously at its own rate (ppm off nominal) but reports its position in chunks
// of `chunk` frames (1 = smooth; VMware HD Audio reports ~1024 at once). The clock thread wakes when
// the pacer says (plus wake-up jitter, optional stalls), ticks, and the Worker writes one block.
// Checks: no underrun, no dropped block, ticks evenly spaced at the device's rate, the chunk learned.
// The old rule (tick whenever the fill is at or below capacity - 2 blocks) is run too, to show the
// bursts it gave on a chunky device, and that the pacer keeps as much queued against stalls.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "HwClockPacer.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("  %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

uint64_t gRng = 88172645463325252ull;
double Uniform() {  // xorshift64, [0, 1)
  gRng ^= gRng << 13;
  gRng ^= gRng >> 7;
  gRng ^= gRng << 17;
  return static_cast<double>(gRng >> 11) / 9007199254740992.0;
}

struct Scenario {
  const char* name;
  double rate;
  int block;
  int capacity;
  int chunk;        // device position report granularity, frames
  double ppm;       // device clock vs nominal
  double stallSec;  // clock thread stalls this long every 2 s (0 = never)
  bool oldRule;     // tick when fill <= capacity - 2 blocks (the first HW-paced clock)
  int prebuffer = 0;  // extra frames the device takes at once 3 ms after start
  // > 0: the device keeps its own buffer this deep, pulling `chunk` frames from ours whenever it has
  // room (VMware HD Audio): at start it pulls faster than it plays until that buffer is full.
  int depth = 0;
  // > 0: the Windows mixer (Shared mode): wakes every mixerPeriod frames of device time (plus up to
  // 5 ms of wake-up jitter) and takes what is due; every lateEverySec one wake is missed. The mixer
  // then stays a period behind for lagSec (live: ~250 ms) and catches up with a double take.
  int mixerPeriod = 0;
  double lateEverySec = 0;
  double lagSec = 0;
  bool tellMixer = false;  // pass mixerPeriod to HwClockPacer::reset (the driver does for Shared)
};

struct Result {
  long ticks = 0;
  long underruns = 0;  // device ran out of frames to play
  long emptyLooks = 0; // depth model: our buffer seen empty (the device took everything)
  long drops = 0;      // a block written without room
  long bursts = 0;     // tick < 0.25 block after the previous one
  double maxGap = 0;   // blocks
  double tickRate = 0; // frames/s over the run after settle
  double meanFill = 0; // reported fill at a tick
  int chunk = 0;
  int expectedFill = 0;
  long chunkChanges = 0;     // after the warm-up: each one moves the setpoint (the Master Clock's phase)
  double maxGapHandover = 0; // blocks, 0.9..1.5 s: the timeline taking over from the warm-up
  double phaseWander = 0;    // frames: max - min of (ticks x block - device frames played), after settle
};

// Spacing and rate are measured after the pacer's 1 s warm-up (it follows the device's fill until then).
constexpr double kSettleSec = 1.5;

Result Run(const Scenario& sc, double seconds) {
  const double devRate = sc.rate * (1.0 + sc.ppm * 1e-6);
  const double blockSec = sc.block / sc.rate;
  HwClockPacer pacer;
  pacer.reset(sc.rate, sc.block, sc.capacity, sc.tellMixer ? sc.mixerPeriod : 0);
  int64_t mixerTaken = 0;
  long mixerWakes = 1;
  double lagUntil = -1.0;
  double mixerWake = sc.mixerPeriod > 0 ? sc.mixerPeriod / devRate : 0.0, mixerLate = sc.lateEverySec;
  int lastChunk = -1;
  double phaseMin = 1e18, phaseMax = -1e18;
  // Started like KsAudio::start: silence prefilled to capacity - 2 blocks.
  int64_t written = sc.capacity - 2 * sc.block;
  double t = 0.0;
  double lastTick = -1.0, settleTick = -1.0;
  long settleTicks = 0;
  double fillSum = 0;
  long fillCount = 0;
  double nextStall = 2.0;
  Result r;
  auto trueConsumed = [&](double at) { return at * devRate + (at >= 0.003 ? sc.prebuffer : 0); };
  auto reportedConsumed = [&](double at) {
    const int64_t c = static_cast<int64_t>(trueConsumed(at));
    return (c / sc.chunk) * sc.chunk;
  };
  int64_t pulled = 0;  // depth model: frames the device has taken from our buffer
  while (t < seconds) {
    int64_t reported;
    if (sc.mixerPeriod > 0) {
      while (t >= mixerWake) {
        if (sc.lateEverySec > 0 && mixerWake >= mixerLate) {
          mixerLate += sc.lateEverySec;  // this wake is missed
          lagUntil = mixerWake + sc.lagSec;
        } else {
          int64_t due = static_cast<int64_t>(mixerWake * devRate / sc.mixerPeriod) * sc.mixerPeriod;
          if (mixerWake < lagUntil) due -= sc.mixerPeriod;  // still a period behind
          if (due > written) ++r.underruns;  // the mixer plays silence for what we had not written
          mixerTaken = due;
          if (written < mixerTaken) written = mixerTaken;
        }
        // Wake-up jitter as live on the dev VM: up to 5 ms late (a missed wake then comes ~25 ms late).
        mixerWake = static_cast<double>(++mixerWakes * sc.mixerPeriod) / devRate + 0.005 * Uniform();
      }
      reported = mixerTaken;
    } else if (sc.depth > 0) {
      const double played = t * devRate;
      if (played > static_cast<double>(pulled)) {  // its own buffer ran dry: a gap
        ++r.underruns;
        pulled = static_cast<int64_t>(played);
        if (pulled > written) written = pulled;
      }
      while (static_cast<double>(pulled) - played < sc.depth && written - pulled >= sc.chunk) pulled += sc.chunk;
      reported = pulled;
    } else {
      reported = reportedConsumed(t);
      if (trueConsumed(t) > static_cast<double>(written)) {  // played past the end: a gap
        ++r.underruns;
        written = static_cast<int64_t>(trueConsumed(t)) + sc.block;  // device resumes on the next write
      }
    }
    const int32_t padding = static_cast<int32_t>(written - reported < 0 ? 0 : written - reported);
    if (sc.depth > 0 && padding == 0 && t > 0.001) ++r.emptyLooks;
    bool tick;
    double wait = 0;
    if (sc.oldRule) {
      tick = padding <= sc.capacity - 2 * sc.block;
      wait = (padding - (sc.capacity - 2 * sc.block)) / devRate;
    } else {
      pacer.observe(t, written, padding);
      wait = pacer.untilTick(t);
      tick = wait <= 0;
    }
    if (t > 1.05) {  // after the warm-up (1 s from the first report)
      if (lastChunk >= 0 && pacer.chunk() != lastChunk) ++r.chunkChanges;
      lastChunk = pacer.chunk();
    }
    if (tick) {
      if (padding + sc.block > sc.capacity) ++r.drops;
      else written += sc.block;  // the Worker's write, right after the tick
      if (lastTick >= 0) {
        const double gap = (t - lastTick) / blockSec;
        if (t > 0.9 && t < kSettleSec && gap > r.maxGapHandover) r.maxGapHandover = gap;
        if (t > kSettleSec) {
          if (gap < 0.25) ++r.bursts;
          if (gap > r.maxGap) r.maxGap = gap;
        }
      }
      if (t > kSettleSec && settleTick < 0) settleTick = t;
      if (t > kSettleSec) {  // the Master Clock against the device's own clock: what a HW input reads
        const double phase = static_cast<double>(r.ticks) * sc.block - t * devRate;
        if (phase < phaseMin) phaseMin = phase;
        if (phase > phaseMax) phaseMax = phase;
      }
      if (settleTick >= 0) {
        ++settleTicks;
        fillSum += padding;
        ++fillCount;
      }
      lastTick = t;
      ++r.ticks;
      t += 0.00002;  // bufferSwitch + Worker hand-off
      continue;
    }
    // Sleep: at most half a block (we poll to see chunks), plus 0..0.3 ms wake-up jitter.
    double sleep = wait < blockSec / 2 ? wait : blockSec / 2;
    if (sleep < 0.0001) sleep = 0.0001;
    t += sleep + 0.0003 * Uniform();
    if (sc.stallSec > 0 && t >= nextStall) {
      t += sc.stallSec;
      nextStall += 2.0;
    }
  }
  r.tickRate = settleTicks > 1 ? (settleTicks - 1) * sc.block / (lastTick - settleTick) : 0;
  r.phaseWander = phaseMax > phaseMin ? phaseMax - phaseMin : 0;
  r.meanFill = fillCount ? fillSum / fillCount : 0;
  r.chunk = pacer.chunk();
  r.expectedFill = pacer.expectedFill();
  return r;
}

void Report(const Scenario& sc, const Result& r) {
  std::printf("%s: %ld ticks, %ld underruns, %ld drops, %ld back-to-back, longest gap %.2f blocks, rate %.1f (device %.1f),"
              " mean fill %.0f (expected %d), chunk %d\n",
              sc.name, r.ticks, r.underruns, r.drops, r.bursts, r.maxGap, r.tickRate, sc.rate * (1 + sc.ppm * 1e-6),
              r.meanFill, r.expectedFill, r.chunk);
}

}  // namespace

int main() {
  const double kSeconds = 20.0;
  {
    const Scenario sc{"smooth device, 48k, HW 128", 48000, 128, 512, 1, 300, 0, false};
    const Result r = Run(sc, kSeconds);
    Report(sc, r);
    check("no underrun, no dropped block", r.underruns == 0 && r.drops == 0);
    check("evenly spaced (no back-to-back ticks, gaps < 1.5 blocks)", r.bursts == 0 && r.maxGap < 1.5);
    check("ticks at the device's rate (within 20 ppm)", std::abs(r.tickRate / (sc.rate * (1 + sc.ppm * 1e-6)) - 1) < 20e-6);
    check("smooth device: no chunk, same threshold as before (capacity - 2 blocks)", r.chunk == 0 && r.expectedFill == 256);
    check("mean fill at a tick = the latency reported (within a block)", std::abs(r.meanFill - r.expectedFill) < sc.block);
  }
  {
    const Scenario sc{"chunky device (1024), 44.1k, HW 512", 44100, 128, 2048, 1024, -200, 0, false};
    const Result r = Run(sc, kSeconds);
    Report(sc, r);
    check("no underrun, no dropped block", r.underruns == 0 && r.drops == 0);
    check("evenly spaced despite the chunks (< 1% back-to-back, gaps < 2 blocks)", r.bursts < r.ticks / 100 && r.maxGap < 2.0);
    check("ticks at the device's rate (within 50 ppm)", std::abs(r.tickRate / (sc.rate * (1 + sc.ppm * 1e-6)) - 1) < 50e-6);
    check("chunk learned (~1024)", r.chunk >= 896 && r.chunk <= 1152);
    check("mean fill at a tick = the latency reported (within a block)", std::abs(r.meanFill - r.expectedFill) < sc.block);
    const Scenario old{"  same device, old rule", 44100, 128, 2048, 1024, -200, 0, true};
    const Result o = Run(old, kSeconds);
    Report(old, o);
    check("the old rule burst on it (what this fixes)", o.bursts > o.ticks / 2);
    check("the old rule reported the wrong latency (its mean fill was not its threshold)", std::abs(o.meanFill - 1792) > sc.block);
    check("no more latency than the old rule (within a block)", r.meanFill < o.meanFill + sc.block);
  }
  {
    const Scenario sc{"chunky device + 17 ms stalls every 2 s, 44.1k, HW 512", 44100, 128, 2048, 1024, 100, 0.017, false};
    const Result r = Run(sc, kSeconds);
    Report(sc, r);
    check("stalls: no underrun, no dropped block", r.underruns == 0 && r.drops == 0);
    check("stalls: back to the device's rate (within 100 ppm)", std::abs(r.tickRate / (sc.rate * (1 + sc.ppm * 1e-6)) - 1) < 100e-6);
  }
  {
    Scenario sc{"chunky device (512) that prebuffers 512 at start, 44.1k, HW 256", 44100, 128, 1024, 512, 150, 0, false};
    sc.prebuffer = 512;
    const Result r = Run(sc, kSeconds);
    Report(sc, r);
    check("prebuffering start: no underrun, no dropped block", r.underruns == 0 && r.drops == 0);
    check("prebuffering start: then evenly spaced (< 1% back-to-back)", r.bursts < r.ticks / 100);
  }
  {
    Scenario sc{"device with its own 1024-frame buffer, pulls 512 at a time (VMware HD Audio), 44.1k, HW 256", 44100, 128,
                1024, 512, 150, 0, false};
    sc.depth = 1024;
    const Result r = Run(sc, kSeconds);
    Report(sc, r);
    std::printf("  (our buffer seen empty %ld times)\n", r.emptyLooks);
    check("own-buffer device: no gap, our buffer never empty after start", r.underruns == 0 && r.emptyLooks == 0);
    check("own-buffer device: evenly spaced after warm-up (< 2% back-to-back)", r.bursts < r.ticks / 50);
  }
  {
    // WASAPI Shared (ADR 0014): the mixer takes 480 frames per 10 ms wake, 960 after a missed one.
    // Learned from the reports, the chunk flips between 512 and 1024 as a double take enters and
    // leaves its window, and each flip moved the Master Clock's phase by 512 frames against the device
    // (live: HW inputs resampled false drift or starved). Known from the start (two mixer periods),
    // it never moves.
    Scenario sc{"Windows mixer (Shared): 480 per wake, a missed wake every 2.3 s, device +300 ppm, 48k, HW 256", 48000, 256, 2880,
                1, 300, 0, false};
    sc.mixerPeriod = 480;
    sc.lateEverySec = 2.3;
    sc.lagSec = 0.25;
    Scenario learned = sc;
    learned.name = "  same device, chunk learned from the reports";
    const Result l = Run(learned, kSeconds);
    Report(learned, l);
    std::printf("  (chunk changes after warm-up: %ld, longest gap at the hand-over %.1f blocks, phase wander %.0f frames)\n",
                l.chunkChanges, l.maxGapHandover, l.phaseWander);
    check("learned: the chunk moves after warm-up (the Master Clock jumps this fixes)", l.chunkChanges >= 2);
    sc.tellMixer = true;
    const Result r = Run(sc, kSeconds);
    Report(sc, r);
    std::printf("  (chunk changes after warm-up: %ld, longest gap at the hand-over %.1f blocks, phase wander %.0f frames)\n",
                r.chunkChanges, r.maxGapHandover, r.phaseWander);
    // A HW input reads the Master Clock against its own device: an exclusive one takes a wander of
    // over ~120 frames (its drift dead band) as drift.
    check("mixer: the clock's phase against the device wanders < 200 frames after settle", r.phaseWander < 200);
    check("mixer: a fraction of the wander with the chunk learned (the clock before)", r.phaseWander * 4 < l.phaseWander);
    check("mixer: no underrun, no dropped block", r.underruns == 0 && r.drops == 0);
    check("mixer: chunk known from the start never moves (no Master Clock jump)", r.chunkChanges == 0 && r.chunk == 1024);
    check("mixer: no pause when the timeline takes over from the warm-up (gap < 3 blocks)", r.maxGapHandover < 3.0);
    check("mixer: ticks at the device's rate (within 100 ppm)", std::abs(r.tickRate / (sc.rate * (1 + sc.ppm * 1e-6)) - 1) < 100e-6);
    // A missed wake leaves the mixer a period behind for a while: a timeline that follows it slows the
    // clock, then hurries after the double take (live: an exclusive HW input read the swing as drift).
    check("mixer: a late wake neither pauses nor hurries the clock (gaps < 1.5 blocks, none back-to-back)",
          r.maxGap < 1.5 && r.bursts == 0);
    check("mixer: mean fill at a tick = the latency reported (within a block)", std::abs(r.meanFill - r.expectedFill) < sc.block);
    check("mixer: no more latency than learning the chunk (within a block)", r.meanFill < l.meanFill + sc.block);
  }
  std::printf("{\"schema_version\":1,\"operation\":\"hw_clock_test\",\"pass\":%s}\n", gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
