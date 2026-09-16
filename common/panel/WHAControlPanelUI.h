#pragma once

// WHAControlPanelUI — panel model for INPUTS 512 (11).
// Vocabulary: Slot, Slot Table, Control Panel, Loopback, Shared Bridge.
// No ImGui required for offline tests — pure model.

#include <cstdint>
#include <string>
#include "WHASlotTable.h"
#include "WHABridgeShared.h"

namespace wha {

enum class PanelFilter : uint32_t {
  All = 0,
  HW = 1,
  Virtual = 2,
  Network = 3,
  Bridge1 = 4,
  Bridge2 = 5,
  Bridge3 = 6,
  Bridge4 = 7,
};

struct PanelModel {
  WHASlotTable table{};
  PanelFilter filter = PanelFilter::All;
  std::string search;
  bool isMaster = true;  // false = Bridge popup filtered to BRIDGE1 only
};

// Filtering
bool MatchesFilter(const WHASlot& slot, PanelFilter filter);
bool MatchesSearch(const WHASlot& slot, const std::string& search);
int FilteredCount(const PanelModel& model, bool isInput);

// Operations on INPUTS only (11)
bool AddInput(PanelModel& model);  // appends SLOT_NONE "- empty -", increments masterInCount
bool InsertEmptyAbove(PanelModel& model, uint32_t index);
bool InsertEmptyBelow(PanelModel& model, uint32_t index);
bool DuplicateInput(PanelModel& model, uint32_t index);
bool DeleteInput(PanelModel& model, uint32_t index);
bool MoveInput(PanelModel& model, uint32_t from, uint32_t to);  // memmove INPUT only
bool SetInputLoopback(PanelModel& model, uint32_t index, bool loopback);  // VIRTUAL only
bool SetInputEnabled(PanelModel& model, uint32_t index, bool enabled);
bool SetInputName(PanelModel& model, uint32_t index, const char* name);
bool SetInputType(PanelModel& model, uint32_t index, WHASlotType type);

// Operations on OUTPUTS only (12) — independent indices
bool AddOutput(PanelModel& model);
bool InsertEmptyAboveOutput(PanelModel& model, uint32_t index);
bool InsertEmptyBelowOutput(PanelModel& model, uint32_t index);
bool DuplicateOutput(PanelModel& model, uint32_t index);
bool DeleteOutput(PanelModel& model, uint32_t index);
bool MoveOutput(PanelModel& model, uint32_t from, uint32_t to);
bool SetOutputLoopback(PanelModel& model, uint32_t index, bool loopback);
bool SetOutputEnabled(PanelModel& model, uint32_t index, bool enabled);
bool SetOutputName(PanelModel& model, uint32_t index, const char* name);
bool SetOutputType(PanelModel& model, uint32_t index, WHASlotType type);

// ABOUT + Save contract (13)
struct AboutInfo {
  std::string version;
  bool sysRunning = false;
  int clsidCount = 5;
  std::string slotsJsonPath;
  std::string bridgeClients[4];
};
AboutInfo GetAboutInfo(const PanelModel& model, WHABridgeShared* bridges[4] = nullptr);
// SavePanel: version++ + memcpy SHM + SerializeSlots to jsonOut + resetRequested (true only for Master Clock change)
// Caller does WriteFile(slots.json), FlushViewOfFile, SetEvent(TableChanged), hostCallback(ASIOResetRequest) if resetRequested
bool SavePanel(PanelModel& editCopy, WHASlotTable* pTable, std::string* jsonOut, bool* resetRequested);
bool ExportSlots(const WHASlotTable& table, const std::string& path);
bool ImportSlots(WHASlotTable& table, const std::string& path, std::string* error);
bool ResetToDefault(PanelModel& model);

// NETWORK 8 tab (16)
bool SetNetworkTx(PanelModel& model, int index, const WHANetworkStream& stream);
bool SetNetworkRx(PanelModel& model, int index, const WHANetworkStream& stream);
WHANetworkStream GetNetworkTx(const PanelModel& model, int index);
WHANetworkStream GetNetworkRx(const PanelModel& model, int index);
double NetworkBandwidthMbps(const WHANetworkStream& stream);  // PCM_F32: ch*48000*32/1e6, PCM_I16: ch*48000*16/1e6, VORBIS: ch*quality*500k/1e6 approx

// GENERAL Virtual Cables (18)
bool SetVirtualCableCount(PanelModel& model, uint32_t count);  // 8 or 64
bool SetVirtualCableName(PanelModel& model, const char* name);
uint32_t GetVirtualCableCount(const PanelModel& model);
std::string GetVirtualCableName(const PanelModel& model);

// GENERAL Per-Thing (12)
bool SetMasterClock(PanelModel& model, uint32_t sampleRate, uint32_t asioBuffer);  // requires host reset
bool SetHwBuffer(PanelModel& model, uint32_t frames);
bool SetVirtualBuffer(PanelModel& model, uint32_t frames);
bool SetBridgeBuffer(PanelModel& model, int bridgeIndex, uint32_t frames);
bool SetNetworkPcmBuffer(PanelModel& model, uint32_t frames);
bool SetNetworkVorbisBuffer(PanelModel& model, uint32_t frames);
bool SetJitterPcm(PanelModel& model, uint32_t ms);
bool SetJitterVorbis(PanelModel& model, uint32_t ms);
bool IsGeneralReadOnly(const PanelModel& model);  // true if !isMaster

// Helpers
const char* SlotDisplayName(const WHASlot& slot);  // "- empty -" for SLOT_NONE else name
bool IsLoopbackEditable(const WHASlot& slot);  // true only if VIRTUAL

}  // namespace wha
