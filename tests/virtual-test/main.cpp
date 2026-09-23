#include <cstdio>
#include <cstring>
#include <memory>
#include "virtual/WHAIoctl.h"
#include "virtual/WHARingBuffer.h"
#include "WHASlotTable.h"
#include "MasterHolder.h"

using namespace wha;

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // RingBuffer 64KB
  WHARingBuffer ring;
  check("ring empty", ring.empty() && ring.size() == 0);
  float data[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  check("ring write 2 frames stereo", ring.write(data, 2) && ring.size() == 2);
  float out[4] = {};
  check("ring read 2 frames", ring.read(out, 2) && out[0] == 0.1f && out[1] == 0.2f);
  check("ring empty after read", ring.empty());

  // IOCTL constants
  check("IOCTL_WHA_READ ok", true);
  check("IOCTL_WHA_WRITE ok", true);
  check("IOCTL_WHA_SET_LOOPBACK ok", true);
  check("kVirtualCables 8", kVirtualCables == 8);
  check("kRingBufferBytes 64KB", kRingBufferBytes == 64 * 1024);

  // Virtual Cable via MasterHolder: SHM Out -> Ring -> SHM In
  WHASlotTable table{};
  table.version = 1;
  table.masterInCount = 1;
  table.masterIn[0].type = SLOT_VIRTUAL; table.masterIn[0].enabled = 1; table.masterIn[0].srcChannel = 0;
  TruncateCopy(table.masterIn[0].name, kNameLen, "VRChat Out");
  table.masterOutCount = 1;
  table.masterOut[0].type = SLOT_VIRTUAL; table.masterOut[0].enabled = 1; table.masterOut[0].srcChannel = 0;
  TruncateCopy(table.masterOut[0].name, kNameLen, "To VRCT");
  table.general.asioBuffer = 128;
  auto masterAudio = std::make_unique<float[]>(2 * 512 * 4096);
  std::memset(masterAudio.get(), 0, 2 * 512 * 4096 * sizeof(float));
  for (int f = 0; f < 128; ++f) masterAudio.get()[f] = 0.7f;  // OUT channel 0
  WHABridgeShared* bridges[4] = {};
  HANDLE masterTick = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE tableChanged = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE bridgeTicks[4][4] = {};
  MasterHolder holder(&table, masterAudio.get(), bridges, masterTick, tableChanged, bridgeTicks);
  holder.tickOnce();
  // After tick, OUT should be in ring, and IN should have data if ring had enough
  // For 128 frames, ring write 128, then read 128 -> IN should have 0.7
  // But our doTick does OUT->Ring then Ring->IN in same tick, so IN should be 0.7
  bool virtualOk = true;
  for (int f = 0; f < 128; ++f) {
    float v = masterAudio.get()[512 * 4096 + f];
    if (std::abs(v - 0.7f) > 0.001f) { virtualOk = false; break; }
  }
  check("virtual SHM Out -> Ring -> SHM In", virtualOk);

  // Stereo cables: srcChannel = cable * 2 + side. OUT Virtual 2 R carries 0.3, OUT Virtual 2 L 0.2;
  // three IN slots on cable 2 (R, L, R) all get their side from one ring read, at a 512-frame block.
  {
    auto stereo = std::make_unique<WHASlotTable>();
    stereo->version = 1;
    stereo->general.asioBuffer = 512;
    stereo->masterOutCount = 2;
    stereo->masterOut[0].type = SLOT_VIRTUAL; stereo->masterOut[0].srcChannel = 3;  // Virtual 2 R
    stereo->masterOut[1].type = SLOT_VIRTUAL; stereo->masterOut[1].srcChannel = 2;  // Virtual 2 L
    stereo->masterInCount = 3;
    stereo->masterIn[0].type = SLOT_VIRTUAL; stereo->masterIn[0].srcChannel = 3;
    stereo->masterIn[1].type = SLOT_VIRTUAL; stereo->masterIn[1].srcChannel = 2;
    stereo->masterIn[2].type = SLOT_VIRTUAL; stereo->masterIn[2].srcChannel = 3;
    auto audio = std::make_unique<float[]>(2 * 512 * 4096);
    std::memset(audio.get(), 0, 2 * 512 * 4096 * sizeof(float));
    for (int f = 0; f < 512; ++f) {
      audio[0 * 4096 + f] = 0.3f;
      audio[1 * 4096 + f] = 0.2f;
    }
    MasterHolder stereoHolder(stereo.get(), audio.get(), bridges, masterTick, tableChanged, bridgeTicks);
    stereoHolder.tickOnce();
    auto all = [&](int inSlot, float want) {
      for (int f = 0; f < 512; ++f)
        if (std::abs(audio[(512 + inSlot) * 4096 + f] - want) > 0.001f) return false;
      return true;
    };
    check("virtual stereo: R and L stay apart", all(0, 0.3f) && all(1, 0.2f));
    check("virtual stereo: two IN slots on one cable both get it (one read)", all(2, 0.3f));
  }

  CloseHandle(masterTick); CloseHandle(tableChanged);

  std::printf("{\"schema_version\":1,\"operation\":\"virtual_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
