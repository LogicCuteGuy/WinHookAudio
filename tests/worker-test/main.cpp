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
  table.masterIn[1].type = SLOT_VIRTUAL; table.masterIn[1].enabled = 1; table.masterIn[1].loopback = 1; table.masterIn[1].srcChannel = 8;  // Virtual 2 L (cable * 8 + channel)
  TruncateCopy(table.masterIn[1].name, kNameLen, "Loopback VRCT");
  table.masterIn[2].type = SLOT_BRIDGE1; table.masterIn[2].enabled = 1; table.masterIn[2].srcChannel = 0;
  TruncateCopy(table.masterIn[2].name, kNameLen, "FL+Live Sum");
  table.masterOutCount = 2;
  table.masterOut[0].type = SLOT_VIRTUAL; table.masterOut[0].enabled = 1; table.masterOut[0].loopback = 1; table.masterOut[0].srcChannel = 8;  // Virtual 2 L
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

  // Bridge 1: two apps running. This tick is block 5; with the default Bridge buffer (one block) the
  // Master mixes the apps' block 5 - 2 = 3, which both have produced (0.5 each on channel 1).
  for (int f = 0; f < 128; ++f) masterAudio[1 * 4096 + f] = 0.25f;  // OUT "To FL+Live" -> the apps' block 5
  auto bridge = std::make_unique<WHABridgeShared>();
  bridge->owner[0] = 100; bridge->owner[1] = 101;
  bridge->owner[2] = 0; bridge->clientBlocks[2] = -1; bridge->clientBlocks[3] = -1;
  bridge->masterBlocks = 5;
  bridge->clientBlocks[0] = 5; bridge->clientBlocks[1] = 5;
  for (int f = 0; f < 128; ++f) {
    bridge->fromClient[0][3][0][f] = 0.5f;
    bridge->fromClient[1][3][0][f] = 0.5f;
    bridge->fromClient[0][4][0][f] = 0.5f;  // next block: only app 0 produced it in time
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

  // Bridge: the apps' block 3 summed with tanh (0.5 + 0.5 -> 0.761) into IN "FL+Live Sum"; the OUT slot
  // is the apps' block 5; block 6 is published.
  bool bridgeOk = true;
  for (int f = 0; f < 128; ++f)
    if (std::abs(masterAudio[512 * 4096 + 2 * 4096 + f] - std::tanh(1.0f)) > 0.001f) { bridgeOk = false; break; }
  check("bridge sum tanh soft-clip (block t - delay)", bridgeOk);
  check("bridge OUT -> the apps' block t, block published",
        std::abs(bridge->toClients[5][0][0] - 0.25f) < 0.001f && bridge->masterBlocks == 6);
  check("bridge broadcast events", WaitForSingleObject(bridgeTicks[0][0], 0) == WAIT_OBJECT_0 &&
                                       WaitForSingleObject(bridgeTicks[0][1], 0) == WAIT_OBJECT_0);
  // Next block (6, mixes block 4): app 1 is late -> only app 0 is heard, app 1 counted late, not repeated.
  bridge->clientBlocks[1] = 4;
  holder.tickOnce();
  check("late bridge app: silence from it, counted late",
        std::abs(masterAudio[512 * 4096 + 2 * 4096] - std::tanh(0.5f)) < 0.001f && bridge->clientLate[1] == 1 &&
            bridge->clientLate[0] == 0 && bridge->masterBlocks == 7);

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
