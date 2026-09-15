#include <cstdio>
#include <cstring>
#include <memory>

#include "WHABridgeShared.h"
#include "WHAPacket.h"
#include "WHASharedMemory.h"
#include "WHASlotTable.h"
#include "WHASlotsJson.h"

namespace {

struct Check {
  const char* name;
  bool pass;
};

bool CheckCounts() {
  if (!wha::IsValidCount(1)) return false;
  if (!wha::IsValidCount(512)) return false;
  if (wha::IsValidCount(0)) return false;
  if (wha::IsValidCount(513)) return false;
  if (wha::IsValidCount(1024)) return false;
  return true;
}

bool CheckEmptySilent() {
  wha::WHASlot empty{};
  // Default slot is NONE + disabled => silent and still counts in template.
  if (!wha::IsSilentSlot(empty)) return false;
  wha::WHASlot hwOff{};
  hwOff.type = wha::SLOT_HW;
  hwOff.enabled = false;
  if (!wha::IsSilentSlot(hwOff)) return false;
  wha::WHASlot hwOn{};
  hwOn.type = wha::SLOT_HW;
  hwOn.enabled = true;
  if (wha::IsSilentSlot(hwOn)) return false;
  return true;
}

bool CheckNameTruncation() {
  wha::WHASlot slot{};
  const char* longName = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-extra-long-name";
  wha::SetSlotName(slot, longName);
  if (slot.name[WHA_NAME_LEN - 1] != '\0') return false;
  if (std::strlen(slot.name) != WHA_NAME_LEN - 1) return false;
  if (std::strncmp(slot.name, longName, WHA_NAME_LEN - 1) != 0) return false;
  wha::WHASlot shortSlot{};
  wha::SetSlotName(shortSlot, "SM58 Mic");
  if (std::strcmp(shortSlot.name, "SM58 Mic") != 0) return false;
  return true;
}

bool CheckLoopbackVirtualOnly() {
  wha::WHASlot def{};
  if (def.loopback) return false;
  if (!wha::IsLoopbackValid(def)) return false;
  wha::WHASlot virt{};
  virt.type = wha::SLOT_VIRTUAL;
  virt.loopback = true;
  if (!wha::IsLoopbackValid(virt)) return false;
  wha::WHASlot hw{};
  hw.type = wha::SLOT_HW;
  hw.loopback = true;
  if (wha::IsLoopbackValid(hw)) return false;
  wha::WHASlot bridge{};
  bridge.type = wha::SLOT_BRIDGE1;
  bridge.loopback = true;
  if (wha::IsLoopbackValid(bridge)) return false;
  wha::WHASlot net{};
  net.type = wha::SLOT_NETWORK;
  net.loopback = true;
  if (wha::IsLoopbackValid(net)) return false;
  return true;
}

bool CheckBridgeInsidePool() {
  if (WHA_MAX != 512) return false;
  if (WHA_BRIDGE_COUNT != 4) return false;
  if (!wha::IsBridgeInsidePool(wha::SLOT_BRIDGE1)) return false;
  if (!wha::IsBridgeInsidePool(wha::SLOT_BRIDGE2)) return false;
  if (!wha::IsBridgeInsidePool(wha::SLOT_BRIDGE3)) return false;
  if (!wha::IsBridgeInsidePool(wha::SLOT_BRIDGE4)) return false;
  if (!wha::IsBridgeInsidePool(wha::SLOT_HW)) return false;
  if (!wha::IsBridgeInsidePool(wha::SLOT_NONE)) return false;
  return true;
}

bool CheckNetworkStreams() {
  if (WHA_NET_STREAMS != 8) return false;
  if (!wha::IsValidCodec(wha::WHA_PCM_F32)) return false;
  if (!wha::IsValidCodec(wha::WHA_PCM_I16)) return false;
  if (!wha::IsValidCodec(wha::WHA_VORBIS)) return false;
  if (wha::IsValidCodec(99)) return false;
  if (!wha::IsValidNetworkChannels(wha::WHA_PCM_F32, 2)) return false;
  if (!wha::IsValidNetworkChannels(wha::WHA_PCM_F32, 32)) return false;
  if (wha::IsValidNetworkChannels(wha::WHA_PCM_F32, 33)) return false;
  if (!wha::IsValidNetworkChannels(wha::WHA_VORBIS, 128)) return false;
  if (wha::IsValidNetworkChannels(wha::WHA_VORBIS, 129)) return false;
  if (wha::IsValidNetworkChannels(wha::WHA_PCM_F32, 0)) return false;
  wha::WHASlotTable table{};
  // Table carries 8 Tx plus 8 Rx streams.
  (void)table;
  return true;
}

bool CheckMasterClockDefaults() {
  wha::WHAGeneral general{};
  if (general.sampleRate != WHA_MASTER_CLOCK_RATE_DEFAULT) return false;
  if (general.bitDepth != WHA_MASTER_CLOCK_BITS_DEFAULT) return false;
  if (general.asioBuffer != WHA_MASTER_CLOCK_BUFFER_DEFAULT) return false;
  if (!wha::IsValidMasterClock(48000, 128)) return false;
  if (!wha::IsValidMasterClock(44100, 64)) return false;
  if (!wha::IsValidMasterClock(96000, 1024)) return false;
  if (wha::IsValidMasterClock(22050, 128)) return false;
  if (wha::IsValidMasterClock(48000, 100)) return false;
  if (general.hwBuffer != 64) return false;
  if (general.virtualBuffer != 256) return false;
  if (general.networkPcmBuffer != 512) return false;
  if (general.networkVorbisBuffer != 1024) return false;
  return true;
}

bool CheckShmNamesUnique() {
  const char* names[] = {
      wha::shm::kSlotTableName,
      wha::shm::kMasterAudioName,
      wha::shm::kBridgeSharedNames[0],
      wha::shm::kBridgeSharedNames[1],
      wha::shm::kBridgeSharedNames[2],
      wha::shm::kBridgeSharedNames[3],
      wha::shm::kMasterTickName,
      wha::shm::kTableChangedName,
      wha::shm::kBridgeTickNames[0][0],
      wha::shm::kBridgeTickNames[0][1],
      wha::shm::kBridgeTickNames[0][2],
      wha::shm::kBridgeTickNames[0][3],
      wha::shm::kBridgeTickNames[1][0],
      wha::shm::kBridgeTickNames[1][1],
      wha::shm::kBridgeTickNames[1][2],
      wha::shm::kBridgeTickNames[1][3],
      wha::shm::kBridgeTickNames[2][0],
      wha::shm::kBridgeTickNames[2][1],
      wha::shm::kBridgeTickNames[2][2],
      wha::shm::kBridgeTickNames[2][3],
      wha::shm::kBridgeTickNames[3][0],
      wha::shm::kBridgeTickNames[3][1],
      wha::shm::kBridgeTickNames[3][2],
      wha::shm::kBridgeTickNames[3][3],
  };
  for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (names[i] == nullptr || names[i][0] == '\0') return false;
    for (std::size_t j = i + 1; j < sizeof(names) / sizeof(names[0]); ++j) {
      if (std::strcmp(names[i], names[j]) == 0) return false;
    }
  }
  return true;
}

bool CheckShmSizes() {
  if (wha::shm::kSlotTableSize != 81920) return false;
  if (wha::shm::kMasterAudioSize != 16 * 1024 * 1024) return false;
  if (wha::shm::kBridgeSharedSize != 8 * 1024 * 1024) return false;
  if (wha::shm::kTotalShmSize !=
      wha::shm::kSlotTableSize + wha::shm::kMasterAudioSize + 4 * wha::shm::kBridgeSharedSize)
    return false;
  // Closing Master DAW unmaps all SHM — silence by design, no stale replay.
  // Offline invariant: total is 48 MB class, not unbounded.
  if (wha::shm::kTotalShmSize != 81920 + 16 * 1024 * 1024 + 32 * 1024 * 1024) return false;
  return true;
}

bool CheckSlotsJsonRoundTrip() {
  auto t = std::make_unique<wha::WHASlotTable>();
  t->version = 1;
  t->masterInCount = 3;
  t->masterIn[0].type = wha::SLOT_HW;
  t->masterIn[0].enabled = true;
  wha::SetSlotName(t->masterIn[0], "SM58 Mic");
  t->masterIn[1].type = wha::SLOT_NONE;
  t->masterIn[1].enabled = false;
  wha::SetSlotName(t->masterIn[1], "- empty -");
  t->masterIn[2].type = wha::SLOT_VIRTUAL;
  t->masterIn[2].enabled = true;
  t->masterIn[2].loopback = true;
  wha::SetSlotName(t->masterIn[2], "Loopback VRCT");
  t->masterOutCount = 2;
  t->masterOut[0].type = wha::SLOT_HW;
  t->masterOut[0].enabled = true;
  wha::SetSlotName(t->masterOut[0], "Main L");
  t->masterOut[1].type = wha::SLOT_VIRTUAL;
  t->masterOut[1].enabled = true;
  t->masterOut[1].loopback = true;
  wha::SetSlotName(t->masterOut[1], "To VRCT");
  t->general.sampleRate = 48000;
  t->general.asioBuffer = 128;
  t->netTx[0].port = 6980;
  t->netTx[0].codec = wha::WHA_VORBIS;
  t->netTx[0].quality = 0.4f;
  t->netTx[0].channels = 2;
  {
    const char* ip = "192.168.1.50";
    std::size_t i = 0;
    for (; i + 1 < sizeof(t->netTx[0].ip) && ip[i] != '\0'; ++i) t->netTx[0].ip[i] = ip[i];
    t->netTx[0].ip[i] = '\0';
  }
  std::string json = wha::SerializeSlots(*t);
  auto out = std::make_unique<wha::WHASlotTable>();
  std::string err;
  if (!wha::DeserializeSlots(json, *out, &err)) return false;
  if (out->version != 1) return false;
  if (out->masterInCount != 3) return false;
  if (out->masterOutCount != 2) return false;
  if (std::strcmp(out->masterIn[0].name, "SM58 Mic") != 0) return false;
  if (std::strcmp(out->masterIn[1].name, "- empty -") != 0) return false;
  if (!out->masterIn[2].loopback) return false;
  if (out->masterIn[2].type != wha::SLOT_VIRTUAL) return false;
  if (std::strcmp(out->masterOut[1].name, "To VRCT") != 0) return false;
  if (out->netTx[0].channels != 2) return false;
  if (out->netTx[0].codec != wha::WHA_VORBIS) return false;
  // Version increments on Save to trigger Master DAW re-query
  wha::IncrementVersion(*out);
  if (out->version != 2) return false;
  return true;
}

bool CheckSlotsJsonRejects() {
  auto t = std::make_unique<wha::WHASlotTable>();
  t->version = 1;
  t->masterInCount = 1;
  t->masterIn[0].type = wha::SLOT_HW;
  t->masterIn[0].enabled = true;
  wha::SetSlotName(t->masterIn[0], "OK");
  t->masterOutCount = 1;
  t->masterOut[0].type = wha::SLOT_HW;
  t->masterOut[0].enabled = true;
  wha::SetSlotName(t->masterOut[0], "OK");
  std::string json = wha::SerializeSlots(*t);
  // Overlong name should be rejected on deserialize
  std::string bad = json;
  // Inject overlong name: replace "OK" with 40-char name
  size_t pos = bad.find("\"OK\"");
  if (pos == std::string::npos) return false;
  bad.replace(pos, 4, "\"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-extra\"");
  auto out = std::make_unique<wha::WHASlotTable>();
  std::string err;
  if (wha::DeserializeSlots(bad, *out, &err)) return false;
  // Loopback on non-virtual should be rejected
  auto t2 = std::make_unique<wha::WHASlotTable>(*t);
  t2->masterIn[0].type = wha::SLOT_HW;
  t2->masterIn[0].loopback = true;
  std::string json2 = wha::SerializeSlots(*t2);
  if (wha::DeserializeSlots(json2, *out, &err)) return false;
  // Out-of-range count
  auto t3 = std::make_unique<wha::WHASlotTable>(*t);
  t3->masterInCount = 0;
  std::string json3 = wha::SerializeSlots(*t3);
  if (wha::DeserializeSlots(json3, *out, &err)) return false;
  return true;
}

bool CheckBridgeRegion() {
  auto b = std::make_unique<wha::WHABridgeShared>();
  if (!wha::IsValidBridgeClientCount(0)) return false;
  if (!wha::IsValidBridgeClientCount(4)) return false;
  if (wha::IsValidBridgeClientCount(5)) return false;
  if (wha::IsValidBridgeClientCount(-1)) return false;
  int32_t id = -1;
  if (!wha::TryAddBridgeClient(*b, &id)) return false;
  if (id != 0 || b->clientCount.load() != 1) return false;
  if (!wha::TryAddBridgeClient(*b, &id)) return false;
  if (!wha::TryAddBridgeClient(*b, &id)) return false;
  if (!wha::TryAddBridgeClient(*b, &id)) return false;
  if (b->clientCount.load() != 4) return false;
  if (wha::TryAddBridgeClient(*b, &id)) return false;  // 5th rejected
  if (!wha::IsValidBridgeReady(0) || !wha::IsValidBridgeReady(1)) return false;
  if (wha::IsValidBridgeReady(2)) return false;
  if (!wha::IsValidBridgeActiveBuf(0) || !wha::IsValidBridgeActiveBuf(1)) return false;
  // Soft-clip preserves loudness: tanh(1.0)=0.761 not 0.5 average
  float samples[4] = {0.5f, 0.5f, 0.0f, 0.0f};
  int32_t ready[4] = {1, 1, 0, 0};
  float mixed = wha::MixBridgeClients(samples, ready, 4);
  if (mixed < 0.7f || mixed > 0.8f) return false;
  float silent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int32_t none[4] = {0, 0, 0, 0};
  if (wha::MixBridgeClients(silent, none, 4) != 0.0f) return false;
  return true;
}

bool CheckWhaaPacket() {
  wha::WHAAPacketHeader h{};
  h.magic = WHAA_MAGIC;
  h.version = WHAA_VERSION;
  h.packetType = wha::WHAA_AUDIO;
  h.codec = wha::WHA_PCM_F32;
  h.channels = 2;
  h.frames = 128;
  h.streamId = 0;
  h.payloadBytes = 2 * 128 * 4;
  std::string err;
  if (!wha::ValidateWhaaHeader(h, &err)) return false;
  h.codec = wha::WHA_PCM_I16;
  h.payloadBytes = 2 * 128 * 2;
  if (!wha::ValidateWhaaHeader(h, &err)) return false;
  h.codec = wha::WHA_VORBIS;
  h.channels = 64;
  h.payloadBytes = 1234;
  if (!wha::ValidateWhaaHeader(h, &err)) return false;
  // Invalid codec
  h.codec = 99;
  if (wha::ValidateWhaaHeader(h, &err)) return false;
  h.codec = wha::WHA_PCM_F32;
  h.channels = 33;  // PCM max 32
  if (wha::ValidateWhaaHeader(h, &err)) return false;
  h.channels = 2;
  h.streamId = 8;  // max 7
  if (wha::ValidateWhaaHeader(h, &err)) return false;
  h.streamId = 0;
  h.payloadBytes = WHAA_MAX_PAYLOAD + 1;
  if (wha::ValidateWhaaHeader(h, &err)) return false;
  // Codebook
  wha::WHAACodebookHeader cb{};
  cb.magic = WHAA_MAGIC;
  cb.version = WHAA_VERSION;
  cb.packetType = wha::WHAA_CODEBOOK;
  cb.streamId = 1;
  cb.sampleRate = 48000;
  cb.channels = 2;
  cb.quality = 0.4f;
  cb.headersBytes = 100;
  if (!wha::ValidateWhaaCodebook(cb, &err)) return false;
  cb.quality = 2.0f;
  if (wha::ValidateWhaaCodebook(cb, &err)) return false;
  return true;
}

}  // namespace

int main() {
  const Check checks[] = {
      {"counts_1_to_512", CheckCounts()},
      {"empty_counts_silent", CheckEmptySilent()},
      {"name_truncation_32", CheckNameTruncation()},
      {"loopback_virtual_only", CheckLoopbackVirtualOnly()},
      {"bridge_inside_pool", CheckBridgeInsidePool()},
      {"network_streams_8", CheckNetworkStreams()},
      {"master_clock_defaults", CheckMasterClockDefaults()},
      {"shm_names_unique", CheckShmNamesUnique()},
      {"shm_sizes_fixed", CheckShmSizes()},
      {"slots_json_roundtrip", CheckSlotsJsonRoundTrip()},
      {"slots_json_rejects", CheckSlotsJsonRejects()},
      {"bridge_region", CheckBridgeRegion()},
      {"whaa_packet", CheckWhaaPacket()},
  };
  bool allPass = true;
  for (const auto& check : checks) {
    if (!check.pass) allPass = false;
  }
  std::printf("{\"schema_version\":1,\"operation\":\"abi_check\",");
  std::printf("\"stream_verified\":false,\"pass\":%s,\"checks\":[",
              allPass ? "true" : "false");
  for (std::size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
    if (i != 0) std::printf(",");
    std::printf("{\"name\":\"%s\",\"pass\":%s}", checks[i].name,
                checks[i].pass ? "true" : "false");
  }
  std::printf("]}\n");
  return allPass ? 0 : 1;
}
