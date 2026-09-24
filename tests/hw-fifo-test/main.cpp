// hw-fifo-test — HwInputFifo against a simulated capture device on its own clock.
// Offline and deterministic: the device delivers packets of `period` frames at its own rate (ppm
// off the Master Clock), optionally late by up to `jitter` ms; the Master Clock reads one block per
// tick. Checks: equal clocks stay a bit-exact copy and never engage the drift loop; drifting clocks
// never starve or trim, hold the fill, estimate the drift, and keep a clean sine; delivery jitter
// grows the target until covered, then stops starving.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "HwInputFifo.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("  %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 128;
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

struct Result {
  uint64_t starved, trims, growths, starvedLate;
  bool engaged;
  double ppm;
  int meanFillLate, expectedFill;
  std::vector<float> out;  // channel 0, every tick
  std::vector<double> ratios;  // per tick
};

// seconds of Master Clock; `tone`: device plays a sine in real time (else index noise).
// missEvery: every that many ticks the Worker misses one (no read; the next read first skips it).
Result Run(double ppm, int period, double jitterMs, double seconds, bool tone, long missEvery = 0) {
  HwInputFifo fifo;
  fifo.reset(kRate, 2, kBlock, period);
  const double devRate = kRate * (1.0 + ppm * 1e-6);
  const long ticks = static_cast<long>(seconds * kRate / kBlock);
  Result r{};
  r.out.reserve(static_cast<size_t>(ticks) * kBlock);
  std::vector<float> packet(static_cast<size_t>(period) * 2), block(2 * kBlock);
  uint64_t produced = 0;  // device frames delivered
  double lastDelivery = 0;
  long k = 0;
  double jitter = Uniform() * jitterMs * 1e-3;  // packet k's lateness, drawn once per packet
  uint64_t starvedAtLate = 0;
  uint64_t fillSumLate = 0, readsLate = 0;
  for (long t = 0; t < ticks; ++t) {
    const double now = static_cast<double>(t) * kBlock / kRate;
    // Deliver every packet that has arrived by now (in order; late packets hold the ones behind).
    while (true) {
      const double complete = static_cast<double>(k + 1) * period / devRate;
      const double delivery = std::max(lastDelivery, complete + jitter);
      if (delivery > now) break;
      lastDelivery = delivery;
      jitter = Uniform() * jitterMs * 1e-3;
      for (int f = 0; f < period; ++f) {
        const uint64_t n = produced + static_cast<uint64_t>(f);
        const float v = tone ? static_cast<float>(0.5 * std::sin(2 * kPi * kToneHz * static_cast<double>(n) / devRate)) : Noise(n);
        packet[static_cast<size_t>(f) * 2] = v;
        packet[static_cast<size_t>(f) * 2 + 1] = -v;
      }
      fifo.push(packet.data(), static_cast<uint32_t>(period), delivery);  // stamped at delivery, as the VM's devices do
      produced += static_cast<uint64_t>(period);
      ++k;
    }
    const uint64_t before = fifo.reads();
    if (missEvery && t % missEvery == missEvery - 1) {  // missed tick: the DAW got a stale block
      r.out.insert(r.out.end(), static_cast<size_t>(kBlock), 0.0f);
      r.ratios.push_back(fifo.ratio());
      fifo.skip(kBlock);
      continue;
    }
    fifo.read(block.data(), kBlock, 2, now);
    r.out.insert(r.out.end(), block.begin(), block.begin() + kBlock);
    r.ratios.push_back(fifo.ratio());
    if (t == ticks / 2) starvedAtLate = fifo.starved();
    if (t > ticks / 2 && fifo.reads() > before) {
      fillSumLate += static_cast<uint64_t>(fifo.fill());
      ++readsLate;
    }
  }
  r.starved = fifo.starved();
  r.starvedLate = fifo.starved() - starvedAtLate;
  r.trims = fifo.trims();
  r.growths = fifo.growths();
  r.engaged = fifo.driftEngaged();
  r.ppm = fifo.sourcePpm();
  r.meanFillLate = readsLate ? static_cast<int>(fillSumLate / readsLate) : -1;
  r.expectedFill = fifo.expectedFill();
  return r;
}

// Tone quality over the last `seconds`, in 20 ms windows: a sine at kToneHz is fitted per window
// (free amplitude and phase), so slow latency wander does not count as error.
//   snr: residual of all windows together, dB (distortion, clicks, repeated/lost frames).
//   wanderFrames: range of the fitted phase across windows, as frames of delay.
struct Tone { double snr, wanderFrames, worstWindowSnr; int badWindows; };
Tone ToneQuality(const std::vector<float>& y, double seconds) {
  const size_t n = y.size(), w = static_cast<size_t>(seconds * kRate), win = static_cast<size_t>(0.02 * kRate);
  double sig = 0, err = 0, phMin = 1e9, phMax = -1e9, phPrev = 0, worst = 1e9;
  int bad = 0;
  bool first = true;
  for (size_t start = n - w; start + win <= n; start += win) {
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t i = start; i < start + win; ++i) {
      const double ph = 2 * kPi * kToneHz * static_cast<double>(i) / kRate, s = std::sin(ph), c = std::cos(ph);
      ss += s * s; sc += s * c; cc += c * c; ys += y[i] * s; yc += y[i] * c;
    }
    const double det = std::max(ss * cc - sc * sc, 1e-12);  // > 0 for a window longer than a period
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double wsig = 0, werr = 0;
    for (size_t i = start; i < start + win; ++i) {
      const double ph = 2 * kPi * kToneHz * static_cast<double>(i) / kRate, fit = a * std::sin(ph) + b * std::cos(ph);
      wsig += fit * fit;
      werr += (y[i] - fit) * (y[i] - fit);
    }
    sig += wsig;
    err += werr;
    const double wsnr = werr > 0 ? 10.0 * std::log10(wsig / werr) : 200.0;
    worst = std::min(worst, wsnr);
    if (wsnr < 60.0) ++bad;
    double phase = std::atan2(b, a);  // unwrap against the previous window
    if (!first) { while (phase - phPrev > kPi) phase -= 2 * kPi; while (phase - phPrev < -kPi) phase += 2 * kPi; }
    first = false;
    phPrev = phase;
    phMin = std::min(phMin, phase);
    phMax = std::max(phMax, phase);
  }
  return {err > 0 ? 10.0 * std::log10(sig / err) : 200.0, (phMax - phMin) / (2 * kPi * kToneHz) * kRate, worst, bad};
}

}  // namespace

int main() {
  std::printf("Equal clocks, no jitter (period 144): bit-exact, loop never engages\n");
  {
    Result r = Run(0, 144, 0, 20, false);
    long delay = -1;
    for (long d = 0; d < 4096 && delay < 0; ++d) {
      bool same = true;
      for (size_t i = r.out.size() - static_cast<size_t>(kRate) * 10; same && i < r.out.size(); ++i)
        same = r.out[i] == Noise(static_cast<uint64_t>(static_cast<long>(i) - d));
      if (same) delay = d;
    }
    std::printf("  delay %ld frames (fill %d, resampler %d)\n", delay, r.expectedFill, HwInputFifo::resamplerDelay());
    check("output == input, sample for sample, over the last 10 s", delay >= 0);
    check("no starve, no trim, not engaged", r.starved == 0 && r.trims == 0 && !r.engaged);
  }

  std::printf("Equal clocks, the Worker misses a tick every 2 s: backlog stays on target, loop never engages\n");
  {
    Result r = Run(0, 144, 0, 30, false, 750);
    std::printf("  starved %llu trims %llu engaged %d; late mean fill %d (expected %d)\n", r.starved, r.trims, r.engaged,
                r.meanFillLate, r.expectedFill);
    check("no starve, no trim, not engaged", r.starved == 0 && r.trims == 0 && !r.engaged);
    check("fill held within 16 frames of expected", std::abs(r.meanFillLate - r.expectedFill) <= 16);
  }

  const struct { double ppm; int period; } drifts[] = {
      {+1000, 144}, {-1000, 144}, {+5000, 144}, {-5000, 144}, {+5000, 512}, {-5000, 488}, {+9000, 512}};
  for (const auto& d : drifts) {
    std::printf("Device %+.0f ppm, period %d, no jitter: 60 s\n", d.ppm, d.period);
    Result r = Run(d.ppm, d.period, 0, 60, true);
    const Tone tone = ToneQuality(r.out, 10);
    {  // ratio variation over the last 10 s: overall range and largest tick-to-tick step, ppm
      double lo = 1e9, hi = -1e9, step = 0;
      for (size_t i = r.ratios.size() - 3750; i < r.ratios.size(); ++i) {
        lo = std::min(lo, r.ratios[i]); hi = std::max(hi, r.ratios[i]);
        step = std::max(step, std::fabs(r.ratios[i] - r.ratios[i - 1]));
      }
      std::printf("  ratio range %.1f ppm, largest step %.2f ppm\n", (hi - lo) * 1e6, step * 1e6);
    }
    std::printf("  starved %llu trims %llu engaged %d estimate %+.1f ppm; late mean fill %d (expected %d); SNR %.1f dB, wander %.2f frames; worst window %.1f dB, %d of 500 < 60 dB\n",
                r.starved, r.trims, r.engaged, r.ppm, r.meanFillLate, r.expectedFill, tone.snr, tone.wanderFrames,
                tone.worstWindowSnr, tone.badWindows);
    check("no starve, no trim", r.starved == 0 && r.trims == 0);
    check("drift estimate within 2% (or 20 ppm)", std::fabs(r.ppm - d.ppm) <= std::max(20.0, 0.02 * std::fabs(d.ppm)));
    check("fill held within 64 frames of expected", std::abs(r.meanFillLate - r.expectedFill) <= 64);
    check("tone SNR >= 60 dB after resampling (20 ms windows)", tone.snr >= 60.0);
    check("latency wander <= 32 frames over the last 10 s", tone.wanderFrames <= 32.0);
  }

  const struct { double ppm; int period; double jitter; } jitters[] = {{0, 139, 17}, {+2000, 139, 17}, {-2000, 488, 17}};
  for (const auto& j : jitters) {
    std::printf("Device %+.0f ppm, period %d, delivery jitter up to %.0f ms: 60 s\n", j.ppm, j.period, j.jitter);
    Result r = Run(j.ppm, j.period, j.jitter, 60, true);
    std::printf("  starved %llu (last 30 s: %llu), target growths %llu, trims %llu, estimate %+.1f ppm\n", r.starved,
                r.starvedLate, r.growths, r.trims, r.ppm);
    check("target grows until jitter is covered, then no starve (last 30 s)", r.starvedLate == 0);
    check("few growths (<= 8)", r.growths <= 8);
  }

  std::printf("{\"schema_version\":1,\"operation\":\"hw_fifo_test\",\"pass\":%s}\n", gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
