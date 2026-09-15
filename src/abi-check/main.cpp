#include <cstdio>
#include <cstring>

#include "../../common/WHASlotTable.h"
#include "../../common/WHASharedMemory.h"

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
