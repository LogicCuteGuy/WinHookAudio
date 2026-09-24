// cable-ring-test — WHACableRing (the Virtual Cable driver's FIFO) the way WinHookAudio.sys uses it.
// Offline and deterministic. A writer and a reader run on their own clocks and block sizes (a WaveRT
// stream copies every millisecond; the Worker exchanges one block per tick). Checks: equal clocks
// pass a bit-exact, gap-free copy after priming, in both directions; drifting clocks stay bounded
// (trims or re-primes, never a growing queue); a full ring keeps the newest frames; an unprimed or
// idle ring gives silence without counting underruns.

#include <cmath>
#include <cstdio>
#include <vector>

#include "virtual/WHACableRing.h"
#include "virtual/WHACableFormat.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("  %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

constexpr double kRate = 48000.0;

struct Result {
  unsigned underruns = 0, drops = 0, maxFill = 0;
  bool continuous = true;  // after the first real frame, every frame read is the next one written
  long framesOut = 0;
};

// The writer puts frame numbers (L = n, R = -n; 1..kWrap, exact in float); the reader checks they come
// out in order. Silence (0) is allowed only before the first frame and around a counted underrun.
constexpr long kWrap = 1L << 22;
float FrameValue(long n) { return static_cast<float>(1 + n % kWrap); }

Result Run(double seconds, unsigned writeBlock, double writePpm, unsigned readBlock, double readPpm,
           unsigned prime, unsigned slack) {
  WHACableRing ring;
  Result r;
  std::vector<float> buf(2 * (writeBlock > readBlock ? writeBlock : readBlock));
  const double writeStep = writeBlock / (kRate * (1.0 + writePpm * 1e-6));
  const double readStep = readBlock / (kRate * (1.0 + readPpm * 1e-6));
  double tw = 0, tr = readStep * 0.37;  // unrelated phases
  long written = 0;
  float expect = 0;  // next frame number expected (0: not started)
  unsigned lastUnderruns = 0, lastDrops = 0;
  while (tw < seconds || tr < seconds) {
    if (tw <= tr) {
      for (unsigned i = 0; i < writeBlock; ++i) {
        buf[2 * i] = FrameValue(written + i);
        buf[2 * i + 1] = -FrameValue(written + i);
      }
      ring.write(buf.data(), writeBlock);
      written += writeBlock;
      if (ring.fill() > r.maxFill) r.maxFill = ring.fill();
      tw += writeStep;
    } else {
      ring.read(buf.data(), readBlock, prime, slack);
      const bool glitch = ring.underruns() != lastUnderruns || ring.drops() != lastDrops;
      lastUnderruns = ring.underruns();
      lastDrops = ring.drops();
      for (unsigned i = 0; i < readBlock; ++i) {
        const float v = buf[2 * i];
        if (buf[2 * i + 1] != -v) r.continuous = false;
        if (v == 0) continue;  // silence: before start, or after an underrun (checked by the counter)
        if (expect != 0 && v != expect && !glitch) r.continuous = false;
        expect = v == static_cast<float>(kWrap) ? 1.0f : v + 1;
      }
      r.framesOut += readBlock;
      tr += readStep;
    }
  }
  r.underruns = ring.underruns();
  r.drops = ring.drops();
  return r;
}

void Report(const char* name, const Result& r) {
  std::printf("%s: underruns %u, drops %u, max fill %u\n", name, r.underruns, r.drops, r.maxFill);
}

}  // namespace

int main() {
  std::printf("cable-ring-test\n");
  {
    // Windows plays into the cable (1 ms copies), the Worker reads 256-frame blocks: prime 2, slack 4 blocks.
    const Result r = Run(60, 48, 0, 256, 0, 512, 1024);
    Report("playback side, equal clocks", r);
    check("gap-free, in order", r.continuous && r.underruns == 0 && r.drops == 0);
    check("queue bounded by prime + a block + a copy", r.maxFill <= 512 + 256 + 48);
  }
  {
    // The Worker writes 256-frame blocks, the recording stream copies 1 ms.
    const Result r = Run(60, 256, 0, 48, 0, 512, 1024);
    Report("recording side, equal clocks", r);
    check("gap-free, in order", r.continuous && r.underruns == 0 && r.drops == 0);
    check("queue bounded by prime + two blocks", r.maxFill <= 512 + 2 * 256);
  }
  {
    // A Worker block larger than a WaveRT copy and odd (DAW buffer 441 at 44.1k).
    const Result r = Run(60, 44, 0, 441, 0, 882, 1764);
    Report("playback side, 441-frame Worker blocks", r);
    check("gap-free, in order", r.continuous && r.underruns == 0 && r.drops == 0);
  }
  {
    // Writer 300 ppm fast: the queue grows ~14 frames/s; trimmed back to prime at prime + slack.
    const Result r = Run(600, 48, 300, 256, 0, 512, 1024);
    Report("writer +300 ppm, 10 min", r);
    check("no underrun", r.underruns == 0);
    check("trimmed: queue never above prime + slack + a copy", r.maxFill <= 512 + 1024 + 48 + 256);
    check("few trims (each drops ~1024 frames)", r.drops > 0 && r.drops <= 600 * 48000 * 300e-6 + 1100);
    check("in order between trims", r.continuous);
  }
  {
    // Writer 300 ppm slow: the queue shrinks 14.4 frames/s; an underrun re-primes (every ~20 s).
    const Result r = Run(600, 48, -300, 256, 0, 512, 1024);
    Report("writer -300 ppm, 10 min", r);
    check("underruns rare (<= one per 13 s)", r.underruns >= 1 && r.underruns <= 45);
    check("no drops", r.drops == 0);
    check("in order between re-primes", r.continuous);
  }
  {
    WHACableRing ring;
    std::vector<float> in(2 * 10000), out(2 * 256, 1.0f);
    ring.read(out.data(), 256, 512, 1024);
    bool silent = true;
    for (float v : out) silent = silent && v == 0.0f;
    check("idle ring: silence, no underrun counted", silent && ring.underruns() == 0);
    for (int i = 0; i < 10000; ++i) in[2 * i] = in[2 * i + 1] = static_cast<float>(i);
    ring.write(in.data(), 10000);
    check("overfull write keeps the newest kFrames", ring.fill() == WHACableRing::kFrames &&
                                                          ring.drops() == 10000 - WHACableRing::kFrames);
    ring.read(out.data(), 1, 0, WHACableRing::kFrames);
    check("oldest kept frame is 10000 - kFrames", out[0] == static_cast<float>(10000 - WHACableRing::kFrames));
    ring.write(nullptr, 4);
    check("null write queues silence", ring.fill() == WHACableRing::kFrames);
    ring.clear();
    check("clear empties and un-primes", ring.fill() == 0 && !ring.primed());
  }
  {
    // Sample conversion (the driver's integer streams): round trips within one step, clipping at full scale.
    const float in[8] = {0.0f, 0.5f, -0.5f, 0.999f, -1.0f, 1.5f, -1.5f, 0.25f};
    unsigned char bytes[8 * 4];
    float out[8];
    bool ok16 = true, ok24 = true, okF = true;
    FloatToSamples(in, bytes, 4, WHASampleKind::Pcm16);
    SamplesToFloat(bytes, out, 4, WHASampleKind::Pcm16);
    for (int i = 0; i < 8; ++i) {
      const float want = in[i] > 1.0f ? 32767.0f / 32768.0f : in[i] < -1.0f ? -1.0f : in[i];
      ok16 = ok16 && std::fabs(out[i] - want) <= 1.0f / 32768.0f;
    }
    FloatToSamples(in, bytes, 4, WHASampleKind::Pcm24);
    SamplesToFloat(bytes, out, 4, WHASampleKind::Pcm24);
    for (int i = 0; i < 8; ++i) {
      const float want = in[i] > 1.0f ? 8388607.0f / 8388608.0f : in[i] < -1.0f ? -1.0f : in[i];
      ok24 = ok24 && std::fabs(out[i] - want) <= 1.0f / 8388608.0f;
    }
    FloatToSamples(in, bytes, 4, WHASampleKind::Float32);
    SamplesToFloat(bytes, out, 4, WHASampleKind::Float32);
    for (int i = 0; i < 8; ++i) okF = okF && out[i] == in[i];
    check("PCM16 round trip within 1 LSB, clipped", ok16);
    check("PCM24 round trip within 1 LSB, clipped", ok24);
    check("float passes bit-exact", okF);
    const float half[2] = {-0.5f, 0.0f};
    unsigned char b16[4];
    FloatToSamples(half, b16, 1, WHASampleKind::Pcm16);
    check("PCM16 -0.5 is 0xC000 little-endian", b16[0] == 0x00 && b16[1] == 0xC0);
    check("frame sizes 4 / 6 / 8 bytes", FrameBytes(WHASampleKind::Pcm16) == 4 && FrameBytes(WHASampleKind::Pcm24) == 6 &&
                                            FrameBytes(WHASampleKind::Float32) == 8);
  }
  std::printf("%s\n", gPass ? "ALL PASS" : "SOME FAILED");
  return gPass ? 0 : 1;
}
