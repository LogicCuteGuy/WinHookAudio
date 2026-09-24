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

  // The Worker's own ring (no driver): frames of 8 channels
  auto ring = std::make_unique<WHARingBuffer>();
  check("ring empty", ring->empty() && ring->size() == 0);
  float data[2 * kVirtualChannels] = {};
  for (uint32_t i = 0; i < 2 * kVirtualChannels; ++i) data[i] = 0.1f * static_cast<float>(i + 1);
  check("ring write 2 frames of 8 channels", ring->write(data, 2) && ring->size() == 2);
  float out[2 * kVirtualChannels] = {};
  check("ring read 2 frames", ring->read(out, 2) && out[0] == data[0] && out[7] == data[7] && out[15] == data[15]);
  check("ring empty after read", ring->empty());

  // IOCTL constants
  check("IOCTL_WHA_READ ok", true);
  check("IOCTL_WHA_WRITE ok", true);
  check("IOCTL_WHA_SET_LOOPBACK ok", true);
  check("kVirtualCables 8", kVirtualCables == 8);
  check("kRingBufferBytes 256KB (8192 frames x 8 ch)", kRingBufferBytes == 256 * 1024);

  // Virtual Cable via MasterHolder: SHM Out -> Ring -> SHM In
  auto table = std::make_unique<WHASlotTable>();
  table->version = 1;
  table->masterInCount = 1;
  table->masterIn[0].type = SLOT_VIRTUAL; table->masterIn[0].enabled = 1; table->masterIn[0].srcChannel = 0;
  TruncateCopy(table->masterIn[0].name, kNameLen, "VRChat Out");
  table->masterOutCount = 1;
  table->masterOut[0].type = SLOT_VIRTUAL; table->masterOut[0].enabled = 1; table->masterOut[0].srcChannel = 0;
  TruncateCopy(table->masterOut[0].name, kNameLen, "To VRCT");
  table->general.asioBuffer = 128;
  auto masterAudio = std::make_unique<float[]>(2 * 512 * 4096);
  std::memset(masterAudio.get(), 0, 2 * 512 * 4096 * sizeof(float));
  for (int f = 0; f < 128; ++f) masterAudio.get()[f] = 0.7f;  // OUT channel 0
  WHABridgeShared* bridges[4] = {};
  HANDLE masterTick = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE tableChanged = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE bridgeTicks[4][4] = {};
  auto holder = std::make_unique<MasterHolder>(table.get(), masterAudio.get(), bridges, masterTick, tableChanged, bridgeTicks);
  holder->tickOnce();
  // doTick does OUT -> Ring then Ring -> IN in the same tick, so IN is 0.7
  bool virtualOk = true;
  for (int f = 0; f < 128; ++f) {
    float v = masterAudio.get()[512 * 4096 + f];
    if (std::abs(v - 0.7f) > 0.001f) { virtualOk = false; break; }
  }
  check("virtual SHM Out -> Ring -> SHM In", virtualOk);

  // srcChannel = cable * 8 + channel. OUT Virtual 2 R carries 0.3, OUT Virtual 2 L 0.2; three IN slots on
  // cable 2 (R, L, R) all get their channel from one ring read, at a 512-frame block.
  auto audio = std::make_unique<float[]>(2 * 512 * 4096);
  auto all = [&](int inSlot, float want) {
    for (int f = 0; f < 512; ++f)
      if (std::abs(audio[(512 + inSlot) * 4096 + f] - want) > 0.001f) return false;
    return true;
  };
  {
    auto stereo = std::make_unique<WHASlotTable>();
    stereo->version = 1;
    stereo->general.asioBuffer = 512;
    stereo->masterOutCount = 2;
    stereo->masterOut[0].type = SLOT_VIRTUAL; stereo->masterOut[0].srcChannel = 1 * 8 + 1;  // Virtual 2 R
    stereo->masterOut[1].type = SLOT_VIRTUAL; stereo->masterOut[1].srcChannel = 1 * 8 + 0;  // Virtual 2 L
    stereo->masterInCount = 3;
    stereo->masterIn[0].type = SLOT_VIRTUAL; stereo->masterIn[0].srcChannel = 9;
    stereo->masterIn[1].type = SLOT_VIRTUAL; stereo->masterIn[1].srcChannel = 8;
    stereo->masterIn[2].type = SLOT_VIRTUAL; stereo->masterIn[2].srcChannel = 9;
    std::memset(audio.get(), 0, 2 * 512 * 4096 * sizeof(float));
    for (int f = 0; f < 512; ++f) {
      audio[0 * 4096 + f] = 0.3f;
      audio[1 * 4096 + f] = 0.2f;
    }
    auto stereoHolder = std::make_unique<MasterHolder>(stereo.get(), audio.get(), bridges, masterTick, tableChanged, bridgeTicks);
    stereoHolder->tickOnce();
    check("virtual stereo: R and L stay apart", all(0, 0.3f) && all(1, 0.2f));
    check("virtual stereo: two IN slots on one cable both get it (one read)", all(2, 0.3f));
  }

  // A 7.1 cable: Ch7 (SL) and Ch8 (SR) carry their own signals; the same channels on a stereo cable
  // are past its count, so those slots are silent.
  {
    auto wide = std::make_unique<WHASlotTable>();
    wide->version = 1;
    wide->general.asioBuffer = 512;
    wide->cables[2].channels = 8;  // Virtual 3: 7.1; Virtual 4 stays stereo
    wide->masterOutCount = 3;
    wide->masterOut[0].type = SLOT_VIRTUAL; wide->masterOut[0].srcChannel = 2 * 8 + 6;  // Virtual 3 Ch7
    wide->masterOut[1].type = SLOT_VIRTUAL; wide->masterOut[1].srcChannel = 2 * 8 + 7;  // Virtual 3 Ch8
    wide->masterOut[2].type = SLOT_VIRTUAL; wide->masterOut[2].srcChannel = 3 * 8 + 6;  // Virtual 4 Ch7: past 2
    wide->masterInCount = 3;
    wide->masterIn[0].type = SLOT_VIRTUAL; wide->masterIn[0].srcChannel = 2 * 8 + 6;
    wide->masterIn[1].type = SLOT_VIRTUAL; wide->masterIn[1].srcChannel = 2 * 8 + 7;
    wide->masterIn[2].type = SLOT_VIRTUAL; wide->masterIn[2].srcChannel = 3 * 8 + 6;
    std::memset(audio.get(), 0, 2 * 512 * 4096 * sizeof(float));
    for (int f = 0; f < 512; ++f) {
      audio[0 * 4096 + f] = 0.4f;
      audio[1 * 4096 + f] = 0.5f;
      audio[2 * 4096 + f] = 0.6f;
      audio[(512 + 2) * 4096 + f] = 9.0f;  // stale: must be cleared
    }
    auto wideHolder = std::make_unique<MasterHolder>(wide.get(), audio.get(), bridges, masterTick, tableChanged, bridgeTicks);
    wideHolder->tickOnce();
    check("virtual 7.1: Ch7 and Ch8 stay apart", all(0, 0.4f) && all(1, 0.5f));
    check("virtual: a channel past the cable's count is silent", all(2, 0.0f));
  }

  CloseHandle(masterTick); CloseHandle(tableChanged);

  std::printf("{\"schema_version\":1,\"operation\":\"virtual_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
