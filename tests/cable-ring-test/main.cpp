// cable-ring-test — WHACableRing (the Virtual Cable driver's FIFO) the way WinHookAudio.sys uses it.
// Offline and deterministic. A writer and a reader run on their own clocks and block sizes (a WaveRT
// stream copies every millisecond; the Worker exchanges one block per tick). Checks: equal clocks
// pass a bit-exact, gap-free copy after priming, in both directions; drifting clocks stay bounded
// (trims or re-primes, never a growing queue); a full ring keeps the newest frames; an unprimed or
// idle ring gives silence without counting underruns; sides of different channel counts share a ring;
// every sample format (16, 24, 24-in-32, 32 PCM, float) converts both ways.

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
      ring.write(buf.data(), writeBlock, 2);
      written += writeBlock;
      if (ring.fill() > r.maxFill) r.maxFill = ring.fill();
      tw += writeStep;
    } else {
      ring.read(buf.data(), readBlock, 2, prime, slack);
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
    ring.read(out.data(), 256, 2, 512, 1024);
    bool silent = true;
    for (float v : out) silent = silent && v == 0.0f;
    check("idle ring: silence, no underrun counted", silent && ring.underruns() == 0);
    for (int i = 0; i < 10000; ++i) in[2 * i] = in[2 * i + 1] = static_cast<float>(i);
    ring.write(in.data(), 10000, 2);
    check("overfull write keeps the newest kFrames", ring.fill() == WHACableRing::kFrames &&
                                                          ring.drops() == 10000 - WHACableRing::kFrames);
    ring.read(out.data(), 1, 2, 0, WHACableRing::kFrames);
    check("oldest kept frame is 10000 - kFrames", out[0] == static_cast<float>(10000 - WHACableRing::kFrames));
    ring.write(nullptr, 4, 2);
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
    FloatToSamples(in, bytes, 8, WHASampleKind::Pcm16);
    SamplesToFloat(bytes, out, 8, WHASampleKind::Pcm16);
    for (int i = 0; i < 8; ++i) {
      const float want = in[i] > 1.0f ? 32767.0f / 32768.0f : in[i] < -1.0f ? -1.0f : in[i];
      ok16 = ok16 && std::fabs(out[i] - want) <= 1.0f / 32768.0f;
    }
    FloatToSamples(in, bytes, 8, WHASampleKind::Pcm24);
    SamplesToFloat(bytes, out, 8, WHASampleKind::Pcm24);
    for (int i = 0; i < 8; ++i) {
      const float want = in[i] > 1.0f ? 8388607.0f / 8388608.0f : in[i] < -1.0f ? -1.0f : in[i];
      ok24 = ok24 && std::fabs(out[i] - want) <= 1.0f / 8388608.0f;
    }
    FloatToSamples(in, bytes, 8, WHASampleKind::Float32);
    SamplesToFloat(bytes, out, 8, WHASampleKind::Float32);
    for (int i = 0; i < 8; ++i) okF = okF && out[i] == in[i];
    bool ok32 = true, ok2432 = true;
    FloatToSamples(in, bytes, 8, WHASampleKind::Pcm32);
    SamplesToFloat(bytes, out, 8, WHASampleKind::Pcm32);
    for (int i = 0; i < 8; ++i) {
      const float want = in[i] > 1.0f ? 1.0f : in[i] < -1.0f ? -1.0f : in[i];
      ok32 = ok32 && std::fabs(out[i] - want) <= 2e-7f;
    }
    FloatToSamples(in, bytes, 8, WHASampleKind::Pcm24in32);
    SamplesToFloat(bytes, out, 8, WHASampleKind::Pcm24in32);
    for (int i = 0; i < 8; ++i) {
      const float want = in[i] > 1.0f ? 8388607.0f / 8388608.0f : in[i] < -1.0f ? -1.0f : in[i];
      ok2432 = ok2432 && std::fabs(out[i] - want) <= 1.0f / 8388608.0f && bytes[4 * i] == 0;
    }
    check("PCM16 round trip within 1 LSB, clipped", ok16);
    check("PCM24 round trip within 1 LSB, clipped", ok24);
    check("PCM32 round trip, clipped at full scale", ok32);
    check("PCM 24-in-32 round trip within 1 LSB, low byte zero", ok2432);
    check("float passes bit-exact", okF);
    const float quarter[1] = {0.25f};
    unsigned char b32[4];
    FloatToSamples(quarter, b32, 1, WHASampleKind::Pcm24in32);
    check("24-in-32: 0.25 is 00 00 00 20 (24 bits in the high bytes)", b32[0] == 0 && b32[1] == 0 && b32[2] == 0 && b32[3] == 0x20);
    const float half[2] = {-0.5f, 0.0f};
    unsigned char b16[4];
    FloatToSamples(half, b16, 1, WHASampleKind::Pcm16);
    check("PCM16 -0.5 is 0xC000 little-endian", b16[0] == 0x00 && b16[1] == 0xC0);
    check("stereo frame sizes 4 / 6 / 8 / 8 / 8 bytes",
          FrameBytes(WHASampleKind::Pcm16, 2) == 4 && FrameBytes(WHASampleKind::Pcm24, 2) == 6 &&
              FrameBytes(WHASampleKind::Float32, 2) == 8 && FrameBytes(WHASampleKind::Pcm32, 2) == 8 &&
              FrameBytes(WHASampleKind::Pcm24in32, 2) == 8);
    check("7.1 24-bit frame is 24 bytes", FrameBytes(WHASampleKind::Pcm24, 8) == 24);
    check("valid bits: 24-in-32 is 24 of 32", ValidBits(WHASampleKind::Pcm24in32) == 24 && SampleBytes(WHASampleKind::Pcm24in32) == 4);
  }
  {
    // Speaker setups the panel offers, and what Windows gets as the channel mask.
    check("channel counts 2/4/6/8 only", IsValidCableChannels(2) && IsValidCableChannels(4) && IsValidCableChannels(6) &&
                                             IsValidCableChannels(8) && !IsValidCableChannels(1) && !IsValidCableChannels(3) &&
                                             !IsValidCableChannels(9));
    check("masks: stereo 0x3, quad 0x33, 5.1 0x3F, 7.1 0x63F",
          CableChannelMask(2) == 0x3 && CableChannelMask(4) == 0x33 && CableChannelMask(6) == 0x3F && CableChannelMask(8) == 0x63F);
    const char* c71 = CableChannelName(8, 6);
    const char* q3 = CableChannelName(4, 2);
    check("names: 7.1 Ch7 is SL, quad Ch3 is BL, past the count none",
          c71 && c71[0] == 'S' && c71[1] == 'L' && q3 && q3[0] == 'B' && q3[1] == 'L' && !CableChannelName(2, 2));
    check("formats 0..4 valid, 5 not", IsValidCableFormat(0) && IsValidCableFormat(4) && !IsValidCableFormat(5));
  }
  {
    // A stereo Windows stream and a 7.1 Worker share one ring: the stream's L/R arrive as Ch1/Ch2 and
    // Ch3..8 are silent; a 7.1 Worker block read by a stereo stream gives its first two channels.
    WHACableRing ring;
    float stereo[2 * 4] = {0.1f, 0.2f, 0.1f, 0.2f, 0.1f, 0.2f, 0.1f, 0.2f};
    ring.write(stereo, 4, 2);
    float wide[8 * 4];
    for (float& v : wide) v = 9.0f;
    ring.read(wide, 4, 8, 0, WHACableRing::kFrames);
    bool ok = true;
    for (int f = 0; f < 4; ++f)
      for (int c = 0; c < 8; ++c) ok = ok && wide[8 * f + c] == (c == 0 ? 0.1f : c == 1 ? 0.2f : 0.0f);
    check("stereo in, 7.1 out: L/R on Ch1/Ch2, the rest silent", ok);
    float block[8 * 4];
    for (int f = 0; f < 4; ++f)
      for (int c = 0; c < 8; ++c) block[8 * f + c] = static_cast<float>(c + 1);
    ring.write(block, 4, 8);
    float two[2 * 4] = {};
    ring.read(two, 4, 2, 0, WHACableRing::kFrames);
    ok = true;
    for (int f = 0; f < 4; ++f) ok = ok && two[2 * f] == 1.0f && two[2 * f + 1] == 2.0f;
    check("7.1 in, stereo out: Ch1/Ch2", ok);
    float six[6 * 4];
    ring.write(block, 4, 8);
    ring.read(six, 4, 6, 0, WHACableRing::kFrames);
    ok = true;
    for (int f = 0; f < 4; ++f)
      for (int c = 0; c < 6; ++c) ok = ok && six[6 * f + c] == static_cast<float>(c + 1);
    check("7.1 in, 5.1 out: Ch1..6 in order", ok);
  }
  std::printf("%s\n", gPass ? "ALL PASS" : "SOME FAILED");
  return gPass ? 0 : 1;
}
