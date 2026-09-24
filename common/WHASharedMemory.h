#pragma once

// WinHookAudio shared-memory names and sizes — offline header-only.
// Vocabulary: Slot Table, Master Clock, Shared Bridge, Network Stream.
// No audio, no devices, no network. Master Driver creates, Bridge Driver opens.
// Closing Master DAW unmaps all SHM — silence by design, no stale replay.

#include <cstddef>

#include "WHABridgeShared.h"

namespace wha::shm {

// Control plane — Slot Table
constexpr const char* kSlotTableName = "Global\\WinHookAudio_SlotTable";
constexpr std::size_t kSlotTableSize = 81920;  // 80 KB class (datasheet: 88,064 B CreateFileMapping)

// Data plane — Master audio ping-pong
constexpr const char* kMasterAudioName = "Global\\WinHookAudio_Master_Audio";
constexpr std::size_t kMasterAudioSize = 16 * 1024 * 1024;  // 16 MB = 2*512*4096*4

// Data plane — Bridge shared (4 bridges, 8 MB each, 32 MB total)
constexpr const char* kBridgeSharedNames[4] = {
    "Global\\WinHookAudio_Bridge1_Shared",
    "Global\\WinHookAudio_Bridge2_Shared",
    "Global\\WinHookAudio_Bridge3_Shared",
    "Global\\WinHookAudio_Bridge4_Shared",
};
constexpr std::size_t kBridgeSharedSize = 8 * 1024 * 1024;  // 8 MB per bridge

// Events — Master tick drives all data
constexpr const char* kMasterTickName = "Global\\WinHookAudio_Master_Tick";
constexpr const char* kTableChangedName = "Global\\WinHookAudio_TableChanged";

// Bridge ticks — 4 bridges × 4 clients = 16 events, Worker broadcasts after summing
constexpr const char* kBridgeTickNames[4][4] = {
    {
        "Global\\WinHookAudio_Bridge1_Tick0",
        "Global\\WinHookAudio_Bridge1_Tick1",
        "Global\\WinHookAudio_Bridge1_Tick2",
        "Global\\WinHookAudio_Bridge1_Tick3",
    },
    {
        "Global\\WinHookAudio_Bridge2_Tick0",
        "Global\\WinHookAudio_Bridge2_Tick1",
        "Global\\WinHookAudio_Bridge2_Tick2",
        "Global\\WinHookAudio_Bridge2_Tick3",
    },
    {
        "Global\\WinHookAudio_Bridge3_Tick0",
        "Global\\WinHookAudio_Bridge3_Tick1",
        "Global\\WinHookAudio_Bridge3_Tick2",
        "Global\\WinHookAudio_Bridge3_Tick3",
    },
    {
        "Global\\WinHookAudio_Bridge4_Tick0",
        "Global\\WinHookAudio_Bridge4_Tick1",
        "Global\\WinHookAudio_Bridge4_Tick2",
        "Global\\WinHookAudio_Bridge4_Tick3",
    },
};

// Persistent files — loaded on Master init, saved on Control Panel Save: routes.yml + settings.yml in
// this folder. slots.json is the older one-file format, read only when neither .yml file exists.
constexpr const char* kConfigDir = "%ProgramData%\\WinHookAudio";
constexpr const char* kRoutesFile = "routes.yml";
constexpr const char* kSettingsFile = "settings.yml";
constexpr const char* kSlotsJsonPath = "%ProgramData%\\WinHookAudio\\slots.json";

// Total pre-allocated on first Master init: 80KB + 16MB + 32MB = 48 MB class
constexpr std::size_t kTotalShmSize = kSlotTableSize + kMasterAudioSize + 4 * kBridgeSharedSize;

static_assert(kSlotTableSize >= 49000 && kSlotTableSize < 81920 + 8192,
              "SlotTable SHM size class");
static_assert(kMasterAudioSize == 16 * 1024 * 1024, "Master audio 16MB");
static_assert(kBridgeSharedSize == 8 * 1024 * 1024, "Bridge shared 8MB per bridge");
// WHABridgeShared (4 clients x 6-block ring + one 6-block ring to them, 64ch x 1024 frames, ~7.9MB) must fit its 8MB view:
// with 4096 frames it was ~18MB and the Worker wrote mixedIn past the mapping on every tick.
static_assert(sizeof(WHABridgeShared) <= kBridgeSharedSize, "WHABridgeShared must fit its SHM view");

}  // namespace wha::shm
