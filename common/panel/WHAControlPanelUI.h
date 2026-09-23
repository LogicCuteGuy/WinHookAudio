#pragma once

// WHAControlPanelUI — panel model for INPUTS 512 (11).
// Vocabulary: Slot, Slot Table, Control Panel, Loopback, Shared Bridge.
// No ImGui required for offline tests — pure model.

#include <cstdint>
#include <string>
#include <vector>
#include "WHASlotTable.h"
#include "WHABridgeShared.h"
#include "WHAMasterStats.h"

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
bool SetInputType(PanelModel& model, uint32_t index, WHASlotType type);  // HW: source reset to L if not L/R
// Source of a slot for its type: HW 0 = L / 1 = R of the GENERAL input device; VIRTUAL cable channel;
// NETWORK Rx stream + channel in it. Names left unset follow it (DawChannelName in WHASlotTable.h).
bool SetInputSource(PanelModel& model, uint32_t index, int32_t srcChannel, int32_t streamId);

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
bool SetOutputSource(PanelModel& model, uint32_t index, int32_t srcChannel, int32_t streamId);

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
bool SetNetworkTx(PanelModel& model, uint32_t index, const WHANetworkStream& stream);
bool SetNetworkRx(PanelModel& model, uint32_t index, const WHANetworkStream& stream);
WHANetworkStream GetNetworkTx(const PanelModel& model, uint32_t index);
WHANetworkStream GetNetworkRx(const PanelModel& model, uint32_t index);
inline double NetworkBandwidthMbps(const WHANetworkStream& stream) noexcept {  // PCM_F32: ch*48000*32/1e6, PCM_I16: ch*48000*16/1e6, VORBIS: ch*quality*500k/1e6 approx
  constexpr double kSampleRate = 48000.0;
  constexpr double kBitsF32 = 32.0;
  constexpr double kBitsI16 = 16.0;
  constexpr double kVorbisMinKbps = 64.0;
  constexpr double kVorbisMaxKbps = 500.0;
  constexpr double kVorbisQualityMin = 0.1;
  constexpr double kVorbisQualityRange = 0.9;
  if (stream.codec == WHA_PCM_F32) return stream.channels * kSampleRate * kBitsF32 / 1e6;
  if (stream.codec == WHA_PCM_I16) return stream.channels * kSampleRate * kBitsI16 / 1e6;
  double kbpsPerStereo = kVorbisMinKbps + (stream.quality - kVorbisQualityMin) / kVorbisQualityRange * (kVorbisMaxKbps - kVorbisMinKbps);
  return stream.channels / 2.0 * kbpsPerStereo / 1000.0;
}

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

// GENERAL HW devices: the endpoints HW slots use. The host enumerates them (MMDevice); the model
// only stores IDs. Empty ID = the Windows default device of that direction.
struct PanelEndpoint {
  std::string id;    // IMMDevice ID (UTF-8)
  std::string name;  // friendly name (UTF-8)
};
struct PanelDevices {
  std::vector<PanelEndpoint> render;
  std::vector<PanelEndpoint> capture;
  std::string defaultRenderId;   // the Windows default devices (what an empty GENERAL ID opens)
  std::string defaultCaptureId;
};
bool SetHwRenderDevice(PanelModel& model, const char* id);   // requires host reset
bool SetHwCaptureDevice(PanelModel& model, const char* id);  // requires host reset
// Friendly name for an ID among `list`; nullptr if the device is not present.
const char* EndpointName(const std::vector<PanelEndpoint>& list, const char* id);
// Friendly name of the device HW slots of one direction use (GENERAL; "" = the Windows default);
// nullptr if unknown.
const char* HwDeviceName(const PanelDevices* devices, const WHAGeneral& general, bool isInput);

// Source menu (INPUTS/OUTPUTS "Source" column): one pick sets type and source together.
// AssignHw also sets the GENERAL device of that direction ("" = Windows default): a direction has one
// HW device, so every HW slot of it follows. Master only (a Bridge popup cannot change GENERAL).
bool AssignSource(PanelModel& model, bool isInput, uint32_t index, WHASlotType type, int32_t srcChannel, int32_t streamId);
bool AssignHw(PanelModel& model, bool isInput, uint32_t index, const char* deviceId, int32_t side);
// What a slot carries, for the Source column: "Microphone · L", "Virtual Cable 2", "Rx3 · Ch1", "Bridge1".
std::string SlotSourceLabel(const WHASlot& slot, bool isInput, const char* hwDevice);

// GENERAL HW status: requested versus actual. `stats` is the streaming Master's (nullptr: the DAW has
// not started the driver); `saved` is the Slot Table's GENERAL now. HW settings are read when the
// DAW starts the driver, so a Save that changed them shows as pending until the DAW resets it.
enum class HwStatusLevel { Info, Ok, Warning, Error };
struct HwStatusLine {
  HwStatusLevel level;
  std::string text;
};
std::vector<HwStatusLine> HwStatusLines(const WHAMasterStats* stats, const WHAGeneral& saved, const PanelDevices* devices);
// Why a HW open failed, in words ("" for an unknown HRESULT).
const char* HwErrorText(int32_t hr);

// Helpers
const char* SlotDisplayName(const WHASlot& slot);  // "- empty -" for SLOT_NONE else name
bool IsLoopbackEditable(const WHASlot& slot);  // true only if VIRTUAL

}  // namespace wha
