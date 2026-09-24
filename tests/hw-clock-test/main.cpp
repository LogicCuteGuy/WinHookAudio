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
};

// Spacing and rate are measured after the pacer's 1 s warm-up (it follows the device's fill until then).
constexpr double kSettleSec = 1.5;

Result Run(const Scenario& sc, double seconds) {
  const double devRate = sc.rate * (1.0 + sc.ppm * 1e-6);
  const double blockSec = sc.block / sc.rate;
  HwClockPacer pacer;
  pacer.reset(sc.rate, sc.block, sc.capacity);
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
    if (sc.depth > 0) {
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
    if (tick) {
      if (padding + sc.block > sc.capacity) ++r.drops;
      else written += sc.block;  // the Worker's write, right after the tick
      if (lastTick >= 0) {
        const double gap = (t - lastTick) / blockSec;
        if (t > kSettleSec) {
          if (gap < 0.25) ++r.bursts;
          if (gap > r.maxGap) r.maxGap = gap;
        }
      }
      if (t > kSettleSec && settleTick < 0) settleTick = t;
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
  std::printf("{\"schema_version\":1,\"operation\":\"hw_clock_test\",\"pass\":%s}\n", gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
