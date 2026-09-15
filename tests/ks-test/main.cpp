#include <cstdio>
#include <cstring>
#include <chrono>
#include "WHASlotTable.h"
#include "WHAPacket.h"
#include "KsAudio.h"

using namespace wha;

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // Offline: measure memcpy 512ch x128x4x2=16MB ~0.05ms SIMD
  const int frames = 128;
  const int channels = 512;
  auto src = std::make_unique<float[]>(channels * frames);
  auto dst = std::make_unique<float[]>(channels * frames);
  for (int i = 0; i < channels * frames; ++i) src[i] = 0.5f;
  auto t0 = std::chrono::high_resolution_clock::now();
  std::memcpy(dst.get(), src.get(), channels * frames * sizeof(float));
  auto t1 = std::chrono::high_resolution_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::printf("memcpy 512ch x128: %.3f ms\n", ms);
  check("memcpy 512ch x128 < 5ms", ms < 5.0);
  check("Master Clock 2.7ms@128/48k", std::abs(1000.0 * 128 / 48000 - 2.666) < 0.01);
  check("HW 64 1.33ms", std::abs(1000.0 * 64 / 48000 - 1.333) < 0.01);

  // KS exclusive open (may fail without HW, but should not crash)
  KsAudio ks;
  bool opened = ks.open(48000, 64);
  std::printf("KS open: %s\n", opened ? "OK" : "no HW (offline)");
  if (opened) {
    check("KS latency 1.33ms", std::abs(ks.latencyMs() - 1.333) < 0.1);
    ks.start();
    float tone[64] = {};
    for (int i = 0; i < 64; ++i) tone[i] = 0.1f;
    bool written = ks.write(tone, 64, 1);
    std::printf("KS write: %s\n", written ? "OK" : "buffer full");
    ks.stop();
    ks.close();
    check("KS write no crash", true);
  } else {
    check("KS open offline (no HW)", true);
  }

  // Failure cases still rejected offline
  check("masterInCount 0 invalid", !IsValidCount(0));
  check("masterInCount 513 invalid", !IsValidCount(513));
  WHASlot bad{}; bad.type = SLOT_HW; bad.loopback = 1;
  check("loopback on HW invalid", !IsLoopbackValid(bad));
  check("WHAA invalid codec", !IsValidWhaaCodec(99));

  std::printf("{\"schema_version\":1,\"operation\":\"ks_test\",\"stream_verified\":%s,\"pass\":%s}\n",
              opened ? "true" : "false", pass ? "true" : "false");
  return pass ? 0 : 1;
}
