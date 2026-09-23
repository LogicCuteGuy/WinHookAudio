// network-test — WHAA Network Streams through the Worker path over UDP loopback (no LAN, no DAW).
// Engine A (Tx) -> 127.0.0.1 -> Engine B (Rx), both driven by processTick() at Master Clock pace
// (128 frames @ 48 kHz = 2.67 ms). Measures end-to-end latency, accuracy and underruns per codec.

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "WHANetwork.h"
#include "WHANetworkEngine.h"

using namespace wha;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 128;
constexpr uint32_t kStride = 4096;
constexpr uint16_t kPortA = 16980;
constexpr uint16_t kPortB = 16981;
const double kPi = 3.14159265358979323846;

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

bool WaitFor(const std::function<bool()>& pred, DWORD ms) {
  const ULONGLONG end = GetTickCount64() + ms;
  while (GetTickCount64() < end) {
    if (pred()) return true;
    Sleep(1);
  }
  return pred();
}

WHASlot NetSlot(int32_t stream, int32_t ch) {
  WHASlot s{};
  s.type = SLOT_NETWORK;
  s.streamId = stream;
  s.srcChannel = ch;
  s.enabled = 1;
  return s;
}

WHANetworkStream Stream(const char* ip, uint16_t port, WHACodec codec, uint32_t channels) {
  WHANetworkStream s{};
  TruncateCopy(s.ip, sizeof(s.ip), ip);
  s.port = port;
  s.codec = codec;
  s.quality = 0.4f;
  s.channels = channels;
  return s;
}

void BaseTable(WHASlotTable& t) {
  t = WHASlotTable{};
  t.general.sampleRate = kRate;
  t.general.asioBuffer = kBlock;
  t.masterInCount = 1;
  t.masterOutCount = 1;
}

// Sender: OUT0/OUT1 -> Tx0 ch0/ch1. Receiver: Rx0 ch0/ch1 -> IN0/IN1.
void SenderTable(WHASlotTable& t, WHACodec codec) {
  BaseTable(t);
  t.masterOutCount = 2;
  t.masterOut[0] = NetSlot(0, 0);
  t.masterOut[1] = NetSlot(0, 1);
  t.netTx[0] = Stream("127.0.0.1", kPortB, codec, 2);
}
void ReceiverTable(WHASlotTable& t, WHACodec codec) {
  BaseTable(t);
  t.masterInCount = 2;
  t.masterIn[0] = NetSlot(0, 0);
  t.masterIn[1] = NetSlot(0, 1);
  t.netRx[0] = Stream("", 6980, codec, 2);
}

// ch0: 5 ms 2 kHz bursts every 0.5 s (latency marker), ch1: continuous 1 kHz sine (accuracy).
float Burst(uint64_t n) {
  const uint64_t period = kRate / 2, len = kRate / 200;
  const uint64_t k = n % period;
  if (n < period || k >= len) return 0.0f;
  return static_cast<float>(0.8 * std::sin(2 * kPi * 2000.0 * static_cast<double>(k) / kRate));
}
float Sine(uint64_t n) { return static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * static_cast<double>(n) / kRate)); }

std::vector<int64_t> Onsets(const std::vector<float>& x, float threshold) {
  std::vector<int64_t> at;
  for (size_t i = 0; i < x.size(); ++i) {
    if (std::abs(x[i]) > threshold && (at.empty() || static_cast<int64_t>(i) - at.back() > kRate / 4))
      at.push_back(static_cast<int64_t>(i));
  }
  return at;
}

struct RunResult {
  int64_t latency = -1;
  bool stable = false;
  double maxErr = 1e9;
  double snrDb = 0;
  uint64_t underrunsAfterPrime = 0;
  uint64_t lost = 0;
  uint64_t txPackets = 0;
  uint64_t rxPackets = 0;
  uint64_t overflows = 0;
  uint64_t trimmed = 0;
  bool driftEngaged = false;
  double driftPpm = 0;
  double fillEarly = 0;  // mean Rx fill, seconds 2..4
  double fillLate = 0;   // mean Rx fill, last 3 s
  double maxStep = 0;    // largest sample-to-sample jump on the sine channel after priming
};

// senderPpm: sender Master Clock offset vs the receiver (+ = sender fast).
RunResult RunLoopback(WHACodec codec, double seconds, uint32_t jitterMs, double senderPpm = 0.0) {
  RunResult r;
  WHASlotTable ta, tb;
  SenderTable(ta, codec);
  ReceiverTable(tb, codec);
  tb.general.jitterPcm = jitterMs;
  tb.general.jitterVorbis = jitterMs;
  WHANetworkEngine a(&ta, {kPortA, 250});
  WHANetworkEngine b(&tb, {kPortB, 250});
  a.start();
  b.start();
  WaitFor([&] { return a.txActive(0) && b.rxActive(0); }, 2000);

  std::vector<float> outA(2 * kStride), inA(kStride), outB(kStride), inB(2 * kStride);
  const uint64_t ticks = static_cast<uint64_t>(seconds * kRate / kBlock);
  std::vector<float> got0, got1;
  got0.reserve(ticks * kBlock);
  got1.reserve(ticks * kBlock);
  LARGE_INTEGER freq, t0, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  uint64_t underrunsAtPrime = 0;
  bool primedOnce = false;
  uint64_t sent = 0;       // sender frames so far
  double senderDue = 0.0;  // fractional sender frames owed
  double fillEarlySum = 0, fillLateSum = 0;
  uint64_t fillEarlyN = 0, fillLateN = 0;
  const uint64_t ticksPerSec = kRate / kBlock;
  for (uint64_t k = 0; k < ticks; ++k) {
    senderDue += kBlock * (1.0 + senderPpm * 1e-6);
    const uint32_t sendFrames = static_cast<uint32_t>(senderDue);
    senderDue -= sendFrames;
    for (uint32_t f = 0; f < sendFrames; ++f) {
      const uint64_t n = sent + f;
      outA[f] = Burst(n);
      outA[kStride + f] = Sine(n);
    }
    sent += sendFrames;
    a.processTick(outA.data(), inA.data(), kStride, sendFrames);
    b.processTick(outB.data(), inB.data(), kStride, kBlock);
    got0.insert(got0.end(), inB.begin(), inB.begin() + kBlock);
    got1.insert(got1.end(), inB.begin() + kStride, inB.begin() + kStride + kBlock);
    if (!primedOnce && b.rxPrimed(0)) {
      primedOnce = true;
      underrunsAtPrime = b.rxCounters(0).underruns.load();
    }
    const double fill = b.rxFillFrames(0);
    if (k >= 2 * ticksPerSec && k < 4 * ticksPerSec) { fillEarlySum += fill; ++fillEarlyN; }
    if (k + 3 * ticksPerSec >= ticks) { fillLateSum += fill; ++fillLateN; }
    // Master Clock pacing
    const int64_t due = t0.QuadPart + static_cast<int64_t>((k + 1) * kBlock * freq.QuadPart / kRate);
    for (;;) {
      QueryPerformanceCounter(&now);
      const int64_t left = due - now.QuadPart;
      if (left <= 0) break;
      if (left > freq.QuadPart / 667) Sleep(1);
      else YieldProcessor();
    }
  }
  r.underrunsAfterPrime = b.rxCounters(0).underruns.load() - underrunsAtPrime;
  r.lost = b.rxCounters(0).lost.load();
  r.txPackets = a.txCounters(0).packets.load();
  r.rxPackets = b.rxCounters(0).packets.load();
  r.overflows = a.txCounters(0).overflows.load() + b.rxCounters(0).overflows.load();
  r.trimmed = b.rxCounters(0).dropped.load();
  r.driftEngaged = b.rxDriftEngaged(0);
  r.driftPpm = b.rxDriftPpm(0);
  r.fillEarly = fillEarlyN ? fillEarlySum / fillEarlyN : 0;
  r.fillLate = fillLateN ? fillLateSum / fillLateN : 0;
  a.stop();
  b.stop();

  std::vector<float> sent0(got0.size());
  for (size_t n = 0; n < sent0.size(); ++n) sent0[n] = Burst(n);
  const auto s = Onsets(sent0, 0.2f), g = Onsets(got0, 0.2f);
  if (!g.empty()) {
    // Match each received burst to the latest sent burst before it.
    std::vector<int64_t> lat;
    for (int64_t on : g) {
      int64_t best = -1;
      for (int64_t so : s)
        if (so <= on) best = so;
      if (best >= 0) lat.push_back(on - best);
    }
    if (!lat.empty()) {
      r.latency = lat.front();
      r.stable = std::all_of(lat.begin(), lat.end(), [&](int64_t v) { return std::llabs(v - lat.front()) <= 64; });
      std::printf("  burst latencies (frames):");
      for (int64_t v : lat) std::printf(" %lld", static_cast<long long>(v));
      std::printf("\n");
    }
  }
  bool started = false;
  for (size_t n = 1; n < got1.size(); ++n) {  // glitch detector: 1 kHz, 0.5 sine moves <= 0.066 per sample
    started = started || std::abs(got1[n]) > 0.25f;
    if (started) r.maxStep = std::max(r.maxStep, static_cast<double>(std::abs(got1[n] - got1[n - 1])));
  }
  if (r.latency >= 0) {
    double err = 0, noise = 0, ref = 0;
    const size_t start = got1.size() / 2;  // second half: well after priming
    for (size_t n = start; n < got1.size(); ++n) {
      const float want = Sine(n - static_cast<size_t>(r.latency));
      const double d = static_cast<double>(got1[n]) - want;
      err = std::max(err, std::abs(d));
      noise += d * d;
      ref += static_cast<double>(want) * want;
    }
    r.maxErr = err;
    r.snrDb = noise > 0 ? 10 * std::log10(ref / noise) : 200.0;
  }
  return r;
}

void Report(const char* name, const RunResult& r, uint32_t jitterMs) {
  std::printf("%s jitter %u ms: latency %lld frames (%.1f ms), stable %s, max err %.2e, SNR %.1f dB, "
              "underruns after prime %llu, lost %llu, overflows %llu, tx %llu / rx %llu packets\n",
              name, jitterMs, static_cast<long long>(r.latency), r.latency * 1000.0 / kRate, r.stable ? "yes" : "no",
              r.maxErr, r.snrDb, static_cast<unsigned long long>(r.underrunsAfterPrime),
              static_cast<unsigned long long>(r.lost), static_cast<unsigned long long>(r.overflows),
              static_cast<unsigned long long>(r.txPackets), static_cast<unsigned long long>(r.rxPackets));
}

void Loopbacks() {
  const RunResult f32 = RunLoopback(WHA_PCM_F32, 2.5, 20);
  Report("PCM_F32", f32, 20);
  const uint32_t target = kRate * 20 / 1000;
  check("PCM_F32 latency = jitter target .. + 2 ticks", f32.latency >= target && f32.latency <= target + 2 * kBlock);
  check("PCM_F32 stable, sample-exact", f32.stable && f32.maxErr < 1e-6);
  check("PCM_F32 no loss/underrun/overflow", f32.lost == 0 && f32.underrunsAfterPrime == 0 && f32.overflows == 0);
  check("Equal clocks: drift compensation stays off", !f32.driftEngaged);

  // Sender clock off by +/-2000 ppm (96 frames/s). Uncompensated, 20 s would drift 1920 frames:
  // trims at +, underruns at -. Compensated: no trims/underruns/glitches, fill held, ppm estimated
  // (the PI loop is still settling at 20 s: ~85% of the offset; ~99.7% after 45 s).
  for (double ppm : {2000.0, -2000.0}) {
    const RunResult d = RunLoopback(WHA_PCM_F32, 20.0, 20, ppm);
    std::printf("PCM_F32 sender %+.0f ppm: engaged %s, estimated %+.0f ppm, fill %.0f -> %.0f frames, "
                "underruns %llu, trimmed %llu, lost %llu, max step %.4f\n",
                ppm, d.driftEngaged ? "yes" : "no", d.driftPpm, d.fillEarly, d.fillLate,
                static_cast<unsigned long long>(d.underrunsAfterPrime), static_cast<unsigned long long>(d.trimmed),
                static_cast<unsigned long long>(d.lost), d.maxStep);
    char name[96];
    std::snprintf(name, sizeof(name), "Drift %+.0f ppm: compensation engaged, estimate within 25%%", ppm);
    check(name, d.driftEngaged && std::abs(d.driftPpm - ppm) <= 0.25 * std::abs(ppm));
    std::snprintf(name, sizeof(name), "Drift %+.0f ppm: no underrun/trim/glitch, fill held within 250 frames", ppm);
    check(name, d.underrunsAfterPrime == 0 && d.trimmed == 0 && d.lost == 0 && d.maxStep < 0.08 &&
                    std::abs(d.fillLate - d.fillEarly) < 250.0);
  }

  const RunResult i16 = RunLoopback(WHA_PCM_I16, 2.5, 20);
  Report("PCM_I16", i16, 20);
  check("PCM_I16 stable, 16-bit accurate", i16.stable && i16.maxErr < 1e-4 && i16.lost == 0);

  if (!VorbisAvailable()) {
    std::printf("VORBIS: skipped (libvorbis not vendored)\n");
    return;
  }
  for (uint32_t jitter : {50u, 100u}) {
    const RunResult v = RunLoopback(WHA_VORBIS, 3.0, jitter);
    Report("VORBIS Q0.4", v, jitter);
    char name[96];
    std::snprintf(name, sizeof(name), "VORBIS jitter %u: bursts arrive, SNR >= 20 dB, no loss", jitter);
    check(name, v.latency > 0 && v.snrDb >= 20.0 && v.lost == 0);
  }
}

void Deterministic() {
  WHASlotTable tb;
  ReceiverTable(tb, WHA_PCM_F32);
  WHANetworkEngine b(&tb, {kPortB, 250});
  b.start();
  check("Rx stream active when an IN slot maps to it", WaitFor([&] { return b.rxActive(0); }, 2000));

  const auto& c = b.rxCounters(0);
  float pcm[2 * 4] = {};
  WHAAPacketHeader h{};
  h.codec = WHA_PCM_F32;
  h.channels = 2;
  h.frames = 4;
  h.streamId = 0;
  h.payloadBytes = sizeof(pcm);
  auto send = [&](uint32_t seq) {
    h.sequence = seq;
    SendWhaaPacket(h, reinterpret_cast<const uint8_t*>(pcm), "127.0.0.1", kPortB);
  };
  send(0);
  send(5);
  check("Sequence gap counted as lost", WaitFor([&] { return c.packets.load() == 2 && c.lost.load() == 4; }, 1000));
  send(3);
  check("Late packet dropped", WaitFor([&] { return c.dropped.load() == 1 && c.packets.load() == 2; }, 1000));

  send(100000);  // sender restarted with a far-away sequence
  send(0);
  check("Sender restart resyncs instead of muting",
        WaitFor([&] { return c.packets.load() == 4 && c.dropped.load() == 1 && c.lost.load() == 4; }, 1000));
  send(1);
  check("Stream continues after resync", WaitFor([&] { return c.packets.load() == 5 && c.lost.load() == 4; }, 1000));

  WHAAPacketHeader bad = h;
  bad.payloadBytes = 7;  // PCM size mismatch
  const uint64_t before = b.unattributedMalformed();
  SendWhaaPacket(bad, reinterpret_cast<const uint8_t*>(pcm), "127.0.0.1", kPortB);
  WHAAPacketHeader wrongMagic = h;
  wrongMagic.magic = 0x12345678;
  SendWhaaPacket(wrongMagic, reinterpret_cast<const uint8_t*>(pcm), "127.0.0.1", kPortB);
  check("Malformed packets counted, not decoded",
        WaitFor([&] { return b.unattributedMalformed() >= before + 2; }, 1000) && c.packets.load() == 5);

  h.codec = WHA_PCM_I16;
  h.payloadBytes = 2 * 4 * 2;
  send(2);
  check("Codec mismatch vs netRx dropped", WaitFor([&] { return c.dropped.load() == 2; }, 1000));
  h.codec = WHA_PCM_F32;
  h.payloadBytes = sizeof(pcm);

  TruncateCopy(tb.netRx[0].ip, sizeof(tb.netRx[0].ip), "10.9.9.9");
  const uint64_t gen = b.configGeneration();
  b.requestReconfigure();
  WaitFor([&] { return b.configGeneration() > gen; }, 1000);
  const auto& c2 = b.rxCounters(0);
  const uint64_t dropped = c2.dropped.load();
  send(7);
  check("Peer filter drops other senders", WaitFor([&] { return c2.dropped.load() == dropped + 1; }, 1000));

  tb.netRx[0].ip[0] = '\0';
  tb.netRx[0].channels = 1;
  const uint64_t gen2 = b.configGeneration();
  b.requestReconfigure();
  check("Reconfigure to 1 channel applies", WaitFor([&] { return b.configGeneration() > gen2 && b.rxActive(0); }, 1000));
  std::vector<float> out(kStride), in(2 * kStride, 1.0f);
  b.processTick(out.data(), in.data(), kStride, kBlock);
  check("Unprimed / out-of-range channels are silent", in[0] == 0.0f && in[kStride] == 0.0f);

  tb.masterIn[0].type = SLOT_NONE;
  tb.masterIn[1].type = SLOT_NONE;
  const uint64_t gen3 = b.configGeneration();
  b.requestReconfigure();
  check("Unmapped Rx stream deactivates", WaitFor([&] { return b.configGeneration() > gen3 && !b.rxActive(0); }, 1000));
  b.stop();

  WHASlotTable ta;
  SenderTable(ta, WHA_PCM_F32);
  ta.netTx[0].ip[0] = '\0';
  WHANetworkEngine a(&ta, {kPortA, 250});
  a.start();
  Sleep(50);
  check("Tx stream without IP stays inactive", !a.txActive(0));
  a.stop();
}

}  // namespace

int main() {
  timeBeginPeriod(1);
  Deterministic();
  Loopbacks();
  timeEndPeriod(1);
  std::printf("{\"schema_version\":1,\"operation\":\"network_test\",\"stream_verified\":false,\"pass\":%s}\n",
              gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
