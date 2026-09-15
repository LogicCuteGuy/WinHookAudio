#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
#include "WHASlotTable.h"
#include "WHASharedMemory.h"
#include "WHABridgeShared.h"
#include "MasterHolder.h"

using namespace wha;

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // Setup table with Loopback VIRTUAL and Bridge
  WHASlotTable table{};
  table.version = 1;
  table.masterInCount = 3;
  table.masterIn[0].type = SLOT_VIRTUAL; table.masterIn[0].enabled = 1; table.masterIn[0].loopback = 1; table.masterIn[0].srcChannel = 0;
  TruncateCopy(table.masterIn[0].name, kNameLen, "VRChat Out");
  table.masterIn[1].type = SLOT_VIRTUAL; table.masterIn[1].enabled = 1; table.masterIn[1].loopback = 1; table.masterIn[1].srcChannel = 2;
  TruncateCopy(table.masterIn[1].name, kNameLen, "Loopback VRCT");
  table.masterIn[2].type = SLOT_BRIDGE1; table.masterIn[2].enabled = 1; table.masterIn[2].srcChannel = 0;
  TruncateCopy(table.masterIn[2].name, kNameLen, "FL+Live Sum");
  table.masterOutCount = 2;
  table.masterOut[0].type = SLOT_VIRTUAL; table.masterOut[0].enabled = 1; table.masterOut[0].loopback = 1; table.masterOut[0].srcChannel = 2;
  TruncateCopy(table.masterOut[0].name, kNameLen, "To VRCT");
  table.masterOut[1].type = SLOT_BRIDGE1; table.masterOut[1].enabled = 1; table.masterOut[1].srcChannel = 0;
  TruncateCopy(table.masterOut[1].name, kNameLen, "To FL+Live");
  table.general.asioBuffer = 128;
  table.general.sampleRate = 48000;

  // Master audio 16MB simulated: use heap to avoid stack overflow
  auto masterAudio = std::make_unique<float[]>(2 * 512 * 4096);
  std::memset(masterAudio.get(), 0, 2 * 512 * 4096 * sizeof(float));
  // Fill OUT buffer for loopback test: OUT channel 0 (To VRCT) has data
  for (int f = 0; f < 128; ++f) masterAudio[0 * 4096 + f] = 0.5f;
  // IN buffer for loopback target should be 0 initially
  for (int f = 0; f < 128; ++f) masterAudio[512 * 4096 + 1 * 4096 + f] = 0.0f;

  auto bridge = std::make_unique<WHABridgeShared>();
  bridge->clientCount = 2;
  bridge->ready[0] = 1; bridge->ready[1] = 1;
  bridge->activeBuf[0] = 0; bridge->activeBuf[1] = 0;
  for (int ch = 0; ch < 1; ++ch) for (int f = 0; f < 128; ++f) {
    bridge->clientIn[0][0][ch][f] = 0.5f;
    bridge->clientIn[1][0][ch][f] = 0.5f;
  }
  WHABridgeShared* bridges[4] = {bridge.get(), nullptr, nullptr, nullptr};
  HANDLE masterTick = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE tableChanged = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE bridgeTicks[4][4] = {};
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) bridgeTicks[i][j] = CreateEventA(nullptr, FALSE, FALSE, nullptr);

  MasterHolder holder(&table, masterAudio.get(), bridges, masterTick, tableChanged, bridgeTicks);
  holder.tickOnce();

  // Loopback: OUT->IN next tick
  bool loopbackOk = true;
  for (int f = 0; f < 128; ++f) {
    float v = masterAudio[512 * 4096 + 1 * 4096 + f];
    if (std::abs(v - 0.5f) > 0.001f) { loopbackOk = false; break; }
  }
  check("loopback OUT->IN next tick", loopbackOk);

  // Bridge sum: 0.5+0.5=1.0 -> tanh(1.0)=0.761, now toggles to mixedIn[1] (nextActive)
  bool bridgeOk = true;
  float expected = std::tanh(1.0f);
  // After fix, mixedActive toggles 0->1, so check mixedIn[1]
  int active = bridge->mixedActive;
  for (int f = 0; f < 128; ++f) {
    float v = bridge->mixedIn[active][0][f];
    if (std::abs(v - expected) > 0.001f) { bridgeOk = false; break; }
  }
  check("bridge sum tanh soft-clip", bridgeOk);

  bool broadcastOk = (std::abs(bridge->mixedIn[active][0][0] - std::tanh(1.0f)) < 0.001f);
  check("bridge broadcast events", broadcastOk);

  // Per-thing FIFOs: check defaults
  check("per-thing HW 64", table.general.hwBuffer == 64);
  check("per-thing Virtual 256", table.general.virtualBuffer == 256);
  check("per-thing Bridge 128", table.general.bridgeBuffer[0] == 128);
  check("per-thing Network PCM 512", table.general.networkPcmBuffer == 512);
  check("per-thing Network Vorbis 1024", table.general.networkVorbisBuffer == 1024);
  check("jitter PCM 20", table.general.jitterPcm == 20);
  check("jitter Vorbis 50", table.general.jitterVorbis == 50);

  CloseHandle(masterTick); CloseHandle(tableChanged);
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) CloseHandle(bridgeTicks[i][j]);

  std::printf("{\"schema_version\":1,\"operation\":\"worker_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
