// hw-out-fifo-test — HwOutputFifo against a simulated output device on its own clock.
// Offline and deterministic: the Master Clock pushes one block per tick (optionally late: Worker
// jitter, stalls, missed ticks); the device plays at its own rate (ppm off the Master Clock) and
// reports its position smoothly, in chunks, or pulls chunks into a buffer of its own (VMware HD Audio).
// Checks: equal clocks stay a bit-exact copy and never engage the drift loop; drifting clocks never
// run the device dry, hold the backlog, estimate the drift and keep a clean sine; chunky devices and
// a late Worker stay gap-free after start.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "HwOutputFifo.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("  %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 128;
constexpr int kCapacity = 1024;  // HW 256 x 4 periods (KsOpenExclusive)
constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 997.0;

float Noise(uint64_t n) {
  uint64_t x = n * 0x9E3779B97F4A7C15ull;
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 29;
  return 0.1f * (static_cast<float>(x >> 40) / static_cast<float>(1ull << 23) - 1.0f);
}

uint64_t gRng = 88172645463325252ull;
double Uniform() {  // xorshift64, [0, 1)
  gRng ^= gRng << 13;
  gRng ^= gRng >> 7;
  gRng ^= gRng << 17;
  return static_cast<double>(gRng >> 11) / static_cast<double>(1ull << 53);
}

struct Scenario {
  const char* name;
  double ppm = 0;         // device clock vs Master Clock
  int chunk = 0;          // > 0: position reported in steps of this many frames
  int depth = 0;          // > 0: the device pulls `chunk` frames into its own buffer this deep
  double jitterMs = 0;    // Worker wakes up to this late each tick
  double stallEverySec = 0;  // > 0: the Worker stalls this often ...
  double stallMs = 17;       // ... this long (this VM: 17 ms), then catches up at once
  long missEvery = 0;     // > 0: every that many ticks the Worker misses one (skip)
  double wobblePpm = 0;   // > 0: the Master Clock's rate wanders by this much (sine) ...
  double wobbleSec = 4;   // ... over this period (a Master Clock on a chunky device, this VM)
  bool tone = true;       // a sine (else index noise, for the bit-exact check)
  bool trace = false;     // print the loop's state every 0.25 s
};

struct Result {
  uint64_t underruns = 0;      // device ran dry (simulated truth)
  uint64_t underrunsLate = 0;  // ... after the first 5 s
  uint64_t gaps = 0, gapsLate = 0, trims = 0, trimsLate = 0;
  bool engaged = false;
  double ppm = 0;
  int meanFillLate = 0, target = 0;
  std::vector<float> played;  // channel 0 as written to the device
  double ratioRangePpm = 0;   // resampling ratio over the last 10 s: max - min
  double ratioStepPpm = 0;    // ... largest tick-to-tick change
};

Result Run(const Scenario& sc, double seconds) {
  HwOutputFifo fifo;
  fifo.reset(kRate, 2, kBlock, kCapacity);
  const double devRate = kRate * (1.0 + sc.ppm * 1e-6);
  const long ticks = static_cast<long>(seconds * kRate / kBlock);
  Result r;
  r.played.reserve(static_cast<size_t>(seconds * devRate) + 8192);
  std::vector<float> block(2 * kBlock), out(2 * (kCapacity + 64 * kBlock));
  // KsAudio::start prefills silence to capacity - 2 blocks.
  int64_t written = kCapacity - 2 * kBlock;
  r.played.insert(r.played.end(), static_cast<size_t>(written), 0.0f);
  double lost = 0;       // frames the device played as silence because it ran dry
  int64_t pulled = 0;    // depth model: frames the device has taken from our buffer
  double nextStall = sc.stallEverySec, stallUntil = 0;
  uint64_t fillSum = 0, fillN = 0, gapsAt5 = 0, trimsAt5 = 0, underrunsAt5 = 0;
  uint64_t n = 0;  // Master Clock frames produced
  double ratioLo = 1e9, ratioHi = -1e9, ratioPrev = 0;
  for (long k = 0; k < ticks; ++k) {
    double t = static_cast<double>(k) * kBlock / kRate + Uniform() * sc.jitterMs * 1e-3;
    if (sc.wobblePpm > 0) {  // rate deviation wobblePpm peak: a phase deviation of ppm * period / 2 pi
      const double nominal = static_cast<double>(k) * kBlock / kRate;
      t += sc.wobblePpm * 1e-6 * sc.wobbleSec / (2 * kPi) * std::sin(2 * kPi * nominal / sc.wobbleSec);
    }
    if (sc.stallEverySec > 0 && t >= nextStall) {
      stallUntil = t + sc.stallMs * 1e-3;
      nextStall += sc.stallEverySec;
    }
    if (t < stallUntil) t = stallUntil;
    // The device up to t: what it has played, and what it reports.
    const double played = t * devRate - lost;
    if (played > static_cast<double>(written)) {  // ran dry: it plays silence until the next write
      ++r.underruns;
      lost += played - static_cast<double>(written);
    }
    const double playedNow = std::min(played, static_cast<double>(written));
    int64_t reported;
    if (sc.depth > 0) {
      while (static_cast<double>(pulled) - playedNow < sc.depth && written - pulled >= sc.chunk) pulled += sc.chunk;
      if (pulled < static_cast<int64_t>(playedNow)) pulled = static_cast<int64_t>(playedNow);
      reported = pulled;
    } else if (sc.chunk > 0) {
      reported = (static_cast<int64_t>(playedNow) / sc.chunk) * sc.chunk;
    } else {
      reported = static_cast<int64_t>(playedNow);
    }
    const int32_t padding = static_cast<int32_t>(written - reported);

    for (int f = 0; f < kBlock; ++f, ++n) {
      const float v = sc.tone ? static_cast<float>(0.5 * std::sin(2 * kPi * kToneHz * static_cast<double>(n) / kRate)) : Noise(n);
      block[f] = v;
      block[kBlock + f] = -v;
    }
    if (sc.missEvery && k % sc.missEvery == sc.missEvery - 1) {  // missed tick: that block is lost
      fifo.skip(kBlock);
      continue;
    }
    fifo.push(block.data(), kBlock, 2);
    const int w = fifo.plan(t, written, padding);
    fifo.pop(out.data(), w);
    for (int f = 0; f < w; ++f) r.played.push_back(out[static_cast<size_t>(f) * 2]);
    written += w;
    if (t >= seconds - 10.0) {
      const double ratio = fifo.ratio();
      ratioLo = std::min(ratioLo, ratio);
      ratioHi = std::max(ratioHi, ratio);
      if (ratioPrev > 0) r.ratioStepPpm = std::max(r.ratioStepPpm, std::fabs(ratio - ratioPrev) * 1e6);
      ratioPrev = ratio;
    }
    if (sc.trace && k % (static_cast<long>(kRate / kBlock) / 4) == 0)
      std::printf("    t %.2f ratio %+.1f ppm backlog %d target %d chunk %d padding %d wrote %d\n", t,
                  (fifo.ratio() - 1.0) * 1e6, fifo.fill(), fifo.target(), fifo.chunk(), padding, w);
    if (t < 5.0) {
      gapsAt5 = fifo.gaps();
      trimsAt5 = fifo.trims();
      underrunsAt5 = r.underruns;
    } else {
      fillSum += static_cast<uint64_t>(fifo.fill());
      ++fillN;
    }
  }
  r.gaps = fifo.gaps();
  r.gapsLate = fifo.gaps() - gapsAt5;
  r.trims = fifo.trims();
  r.trimsLate = fifo.trims() - trimsAt5;
  r.underrunsLate = r.underruns - underrunsAt5;
  r.engaged = fifo.driftEngaged();
  r.ppm = fifo.devicePpm();
  r.meanFillLate = fillN ? static_cast<int>(fillSum / fillN) : -1;
  r.target = fifo.target();
  r.ratioRangePpm = (ratioHi - ratioLo) * 1e6;
  return r;
}

// Tone quality over the last `seconds` of what the device played, in 20 ms windows (sine fitted per
// window, free amplitude and phase), at the device's rate: the tone is kToneHz in real time.
struct Tone { double snr, wanderFrames, worstWindowSnr; int badWindows; };
Tone ToneQuality(const std::vector<float>& y, double seconds, double rate) {
  const size_t n = y.size(), w = static_cast<size_t>(seconds * rate), win = static_cast<size_t>(0.02 * rate);
  double sig = 0, err = 0, phMin = 1e9, phMax = -1e9, phPrev = 0, worst = 1e9;
  int bad = 0;
  bool first = true;
  for (size_t start = n - w; start + win <= n; start += win) {
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t i = start; i < start + win; ++i) {
      const double ph = 2 * kPi * kToneHz * static_cast<double>(i) / rate, s = std::sin(ph), c = std::cos(ph);
      ss += s * s; sc += s * c; cc += c * c; ys += y[i] * s; yc += y[i] * c;
    }
    const double det = std::max(ss * cc - sc * sc, 1e-12);
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double wsig = 0, werr = 0;
    for (size_t i = start; i < start + win; ++i) {
      const double ph = 2 * kPi * kToneHz * static_cast<double>(i) / rate, fit = a * std::sin(ph) + b * std::cos(ph);
      wsig += fit * fit;
      werr += (y[i] - fit) * (y[i] - fit);
    }
    sig += wsig;
    err += werr;
    const double wsnr = werr > 0 ? 10.0 * std::log10(wsig / werr) : 200.0;
    worst = std::min(worst, wsnr);
    if (wsnr < 60.0) ++bad;
    double phase = std::atan2(b, a);
    if (!first) { while (phase - phPrev > kPi) phase -= 2 * kPi; while (phase - phPrev < -kPi) phase += 2 * kPi; }
    first = false;
    phPrev = phase;
    phMin = std::min(phMin, phase);
    phMax = std::max(phMax, phase);
  }
  return {err > 0 ? 10.0 * std::log10(sig / err) : 200.0, (phMax - phMin) / (2 * kPi * kToneHz) * rate, worst, bad};
}

void Report(const Scenario& sc, const Result& r) {
  std::printf("  underruns %llu (after 5 s: %llu), gaps %llu (%llu), trims %llu (%llu), engaged %d, estimate %+.1f ppm, "
              "late mean backlog %d (target %d)\n",
              static_cast<unsigned long long>(r.underruns), static_cast<unsigned long long>(r.underrunsLate),
              static_cast<unsigned long long>(r.gaps), static_cast<unsigned long long>(r.gapsLate),
              static_cast<unsigned long long>(r.trims), static_cast<unsigned long long>(r.trimsLate), r.engaged, r.ppm,
              r.meanFillLate, r.target);
  std::printf("  ratio over the last 10 s: range %.1f ppm, largest step %.2f ppm\n", r.ratioRangePpm, r.ratioStepPpm);
  (void)sc;
}

}  // namespace

int main() {
  {
    Scenario sc{"equal clocks, smooth device"};
    sc.tone = false;
    std::printf("%s: bit-exact, loop never engages (20 s)\n", sc.name);
    const Result r = Run(sc, 20);
    Report(sc, r);
    long delay = -1;
    const size_t start = r.played.size() - static_cast<size_t>(kRate) * 10;
    for (long d = 0; d < 8192 && delay < 0; ++d) {
      bool same = true;
      for (size_t i = start; same && i < r.played.size(); ++i)
        same = r.played[i] == Noise(static_cast<uint64_t>(static_cast<long>(i) - d));
      if (same) delay = d;
    }
    std::printf("  delay %ld frames (target %d, resampler %d)\n", delay, r.target, HwOutputFifo::resamplerDelay());
    check("played == pushed, sample for sample, over the last 10 s", delay >= 0);
    check("no underrun, gap or trim; not engaged", r.underruns == 0 && r.gaps == 0 && r.trims == 0 && !r.engaged);
  }

  {
    Scenario sc{"equal clocks, the Worker misses a tick every 2 s"};
    sc.missEvery = 750;
    std::printf("%s: backlog stays on target, loop never engages (30 s)\n", sc.name);
    const Result r = Run(sc, 30);
    Report(sc, r);
    check("no underrun, not engaged", r.underruns == 0 && !r.engaged);
    check("backlog held within 32 frames of target", std::abs(r.meanFillLate - r.target) <= 32);
  }

  const struct { double ppm; int chunk; } drifts[] = {{+100, 0}, {-100, 0}, {+1000, 0}, {-1000, 0}, {+5000, 0},
                                                     {-5000, 0}, {+1000, 512}, {-1000, 512}};
  for (const auto& d : drifts) {
    Scenario sc{"drift"};
    sc.ppm = d.ppm;
    sc.chunk = d.chunk;
    std::printf("Device %+.0f ppm, position %s: 60 s\n", d.ppm, d.chunk ? "in 512-frame steps" : "smooth");
    const Result r = Run(sc, 60);
    Report(sc, r);
    const Tone tone = ToneQuality(r.played, 10, kRate * (1.0 + d.ppm * 1e-6));
    std::printf("  SNR %.1f dB, wander %.2f frames; worst window %.1f dB, %d of 500 < 60 dB\n", tone.snr,
                tone.wanderFrames, tone.worstWindowSnr, tone.badWindows);
    check("no underrun, gap or trim after 5 s", r.underrunsLate == 0 && r.gapsLate == 0 && r.trimsLate == 0);
    check("drift estimate within 2% (or 20 ppm)", std::fabs(r.ppm - d.ppm) <= std::max(20.0, 0.02 * std::fabs(d.ppm)));
    check("backlog held within 64 frames of target", std::abs(r.meanFillLate - r.target) <= 64);
    if (!d.chunk) {
      check("tone SNR >= 60 dB after resampling (20 ms windows)", tone.snr >= 60.0);
      check("latency wander <= 32 frames over the last 10 s", tone.wanderFrames <= 32.0);
    } else {
      // Its position is known only to within a step: the loop holds the pitch within about a cent
      // (1000 ppm = 1.7 cents) and moves it slowly, instead of chasing the step.
      check("ratio within 1000 ppm over the last 10 s, no step over 5 ppm",
            r.ratioRangePpm <= 1000.0 && r.ratioStepPpm <= 5.0);
      check("tone SNR >= 40 dB (20 ms windows)", tone.snr >= 40.0);
      check("latency wander <= 64 frames over the last 10 s", tone.wanderFrames <= 64.0);
    }
  }

  {
    Scenario sc{"device with its own 1024-frame buffer, pulls 512 at a time (VMware HD Audio), +300 ppm"};
    sc.ppm = 300;
    sc.chunk = 512;
    sc.depth = 1024;
    std::printf("%s: 60 s\n", sc.name);
    const Result r = Run(sc, 60);
    Report(sc, r);
    const Tone tone = ToneQuality(r.played, 10, kRate * (1.0 + sc.ppm * 1e-6));
    std::printf("  SNR %.1f dB, wander %.2f frames\n", tone.snr, tone.wanderFrames);
    check("no underrun, gap or trim after 5 s", r.underrunsLate == 0 && r.gapsLate == 0 && r.trimsLate == 0);
    check("drift estimate within 20 ppm", std::fabs(r.ppm - sc.ppm) <= 20.0);
    check("ratio within 1000 ppm over the last 10 s, no step over 5 ppm", r.ratioRangePpm <= 1000.0 && r.ratioStepPpm <= 5.0);
    check("tone SNR >= 40 dB", tone.snr >= 40.0);
  }

  {
    Scenario sc{"Worker up to 3 ms late each tick, -500 ppm"};
    sc.ppm = -500;
    sc.jitterMs = 3;
    std::printf("%s: 60 s\n", sc.name);
    const Result r = Run(sc, 60);
    Report(sc, r);
    check("no underrun, gap or trim after 5 s", r.underrunsLate == 0 && r.gapsLate == 0 && r.trimsLate == 0);
    check("drift estimate within 20 ppm", std::fabs(r.ppm - sc.ppm) <= 20.0);
    // The backlog is seen through the Worker's wake-up jitter; the settled (16 s) loop averages it.
    check("ratio within 200 ppm over the last 10 s", r.ratioRangePpm <= 200.0);
  }

  {
    Scenario sc{"Master Clock wandering +-2500 ppm over 4 s (a chunky clock device, this VM), -150 ppm"};
    sc.ppm = -150;
    sc.wobblePpm = 2500;
    std::printf("%s: 60 s\n", sc.name);
    const Result r = Run(sc, 60);
    Report(sc, r);
    // The backlog absorbs the wander (2500 ppm over 4 s moves it by ~76 frames); the pitch follows
    // only the average.
    check("no underrun, gap or trim after 5 s", r.underrunsLate == 0 && r.gapsLate == 0 && r.trimsLate == 0);
    check("drift estimate within 50 ppm (it averages 5 s of a 4 s wander)", std::fabs(r.ppm - sc.ppm) <= 50.0);
    check("ratio within 500 ppm over the last 10 s", r.ratioRangePpm <= 500.0);
  }

  {
    Scenario sc{"Worker stalls 12 ms every 5 s, +200 ppm"};
    sc.ppm = 200;
    sc.stallEverySec = 5;
    sc.stallMs = 12;
    std::printf("%s: 60 s\n", sc.name);
    const Result r = Run(sc, 60);
    Report(sc, r);
    // 12 ms is 576 frames: less than the device holds before a tick (~768), so it never runs dry.
    check("no underrun, gap or trim after 5 s", r.underrunsLate == 0 && r.gapsLate == 0 && r.trimsLate == 0);
    check("drift estimate within 20 ppm", std::fabs(r.ppm - sc.ppm) <= 20.0);
  }

  {
    Scenario sc{"Worker stalls 17 ms every 5 s (this VM), +200 ppm"};
    sc.ppm = 200;
    sc.stallEverySec = 5;
    std::printf("%s: 60 s\n", sc.name);
    const Result r = Run(sc, 60);
    Report(sc, r);
    // 17 ms is 816 frames: the device runs dry for a moment (as the Master Clock's own device would);
    // the catch-up refills it, so no silence of ours and no frames thrown away on top. (The time lost
    // each time here happens to cancel the drift, so the loop never needs to engage.)
    check("no gap or trim after 5 s", r.gapsLate == 0 && r.trimsLate == 0);
    check("backlog held within 64 frames of target", std::abs(r.meanFillLate - r.target) <= 64);
  }

  {
    // The device already plays silence (the Worker held it while the Master Clock settled) and is low
    // when the FIFO starts: blocks alone keep the backlog under half the target, which is refilled and
    // counted as a gap; silence primed up to the target joins seamlessly.
    std::printf("start on a device already playing, fill 200 of %d\n", kCapacity);
    auto gapsAfterStart = [](bool prime) {
      HwOutputFifo fifo;
      fifo.reset(kRate, 2, kBlock, kCapacity);
      int64_t written = 5000;
      int32_t padding = 200;
      if (prime) fifo.primeSilence(padding);
      std::vector<float> block(2 * kBlock, 0.0f), frames(2 * kCapacity);
      for (int tick = 0; tick < 200; ++tick) {  // ~0.5 s
        padding = padding > kBlock ? padding - kBlock : 0;  // the device plays a block per tick
        fifo.push(block.data(), kBlock, 2);
        const int n = fifo.plan(tick * kBlock / kRate, written, padding);
        if (n > 0) {
          fifo.pop(frames.data(), n);
          written += n;
          padding += n;
        }
      }
      return fifo.gaps();
    };
    check("without priming: a gap", gapsAfterStart(false) > 0);
    check("primed with silence: no gap", gapsAfterStart(true) == 0);
  }

  std::printf("{\"schema_version\":1,\"operation\":\"hw_out_fifo_test\",\"pass\":%s}\n", gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
