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
bool SetInputType(PanelModel& model, uint32_t index, WHASlotType type);  // HW: source reset to L / device 0 if invalid
// Source of a slot for its type: HW 0 = L / 1 = R, streamId = the HW device (0..3, listed); VIRTUAL
// cable channel; NETWORK Rx stream + channel in it. Names left unset follow it (DawChannelName).
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
  bool sysRunning = false;       // WinHookAudio.sys (Virtual Cable driver) is loaded
  std::string sysStatus;         // "running", "installed, not running", "not installed"
  int clsidCount = 5;
  std::string configPath;  // where Save writes routes.yml + settings.yml
  std::string bridgeClients[4];
};
// Virtual Cable driver state from the Windows service list (service "WinHookAudio"); checked at most
// every 2 s, so calling it every frame is cheap. status (optional): text for ABOUT.
bool CableDriverRunning(std::string* status = nullptr);
AboutInfo GetAboutInfo(const PanelModel& model, WHABridgeShared* bridges[4] = nullptr);
// SavePanel: version++ + memcpy SHM + routes.yml / settings.yml text (WHAConfigYaml.h) + resetRequested
// (true only for Master Clock change).
// Caller writes the files, FlushViewOfFile, SetEvent(TableChanged), hostCallback(ASIOResetRequest) if resetRequested
bool SavePanel(PanelModel& editCopy, WHASlotTable* pTable, std::string* routesOut, std::string* settingsOut,
               bool* resetRequested);
// Export one part: routes.yml (the slots) or settings.yml (GENERAL), or both in one .yml.
bool ExportRoutes(const WHASlotTable& table, const std::string& path);
bool ExportSettings(const WHASlotTable& table, const std::string& path);
bool ExportEverything(const WHASlotTable& table, const std::string& path);
// Import a routes.yml, a settings.yml, an Everything .yml or an old slots.json into `table`: only the
// part the file holds is replaced. `what` gets "routes", "settings" or "routes and settings". On failure `table` is unchanged.
bool ImportConfig(WHASlotTable& table, const std::string& path, std::string* what, std::string* error);
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
// Per cable: channels (2, 4, 6, 8) and sample format (WHACableSetting::format, 0..4); the rate is the
// Master Clock's. Live: the Worker sends them to the driver, which switches the cable's Windows
// endpoints; the DAW does not reset. A VIRTUAL slot on a channel past the new count goes silent.
bool SetCableChannels(PanelModel& model, int cable, uint32_t channels);
bool SetCableFormat(PanelModel& model, int cable, uint32_t format);
const char* CableChannelsLabel(uint32_t channels);  // "Stereo", "Quad", "5.1", "7.1"
const char* CableFormatLabel(uint32_t format);      // "32-bit float", "16-bit", "24-bit", "32-bit", "24-bit in 32"

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
// Friendly name of HW device 0 of one direction (GENERAL; "" = the Windows default); nullptr if unknown.
const char* HwDeviceName(const PanelDevices* devices, const WHAGeneral& general, bool isInput);
// Friendly names of all HW devices of one direction, by device index (nullptr: not listed / unknown).
void HwDeviceNames(const PanelDevices* devices, const WHASlotTable& table, bool isInput, const char* out[kHwDevices]);

// GENERAL HW device list, per direction (WHAHwMore): device 0 (output 0 is the Master Clock) plus up
// to three more, each on its own clock. Master only; each change asks the DAW to reset on Save.
// The listed device with this ID (-1: none). Device 0 matches "" (Windows default).
int FindHwDevice(const WHASlotTable& table, bool isInput, const char* id);
// HW slots of one direction that use device d.
int HwDeviceSlotCount(const WHASlotTable& table, bool isInput, int device);
// Set device d's ID: "" only for device 0; an ID another device of that direction has is rejected.
bool SetHwDevice(PanelModel& model, bool isInput, int device, const char* id);
// Add a device (the first free index 1..3); returns its index, -1 if full, listed already or "".
int AddHwDevice(PanelModel& model, bool isInput, const char* id);
// Remove device 1..3 from the list; its HW slots become empty.
bool RemoveHwDevice(PanelModel& model, bool isInput, int device);

// Source menu (INPUTS/OUTPUTS "Source" column): one pick sets type and source together.
// AssignHw picks a device by ID: a listed one; else it replaces device 0 when no other HW slot uses
// it (switching the one device), else it is added to the list (false when the list is full).
// Master only (a Bridge popup cannot change GENERAL).
bool AssignSource(PanelModel& model, bool isInput, uint32_t index, WHASlotType type, int32_t srcChannel, int32_t streamId);
bool AssignHw(PanelModel& model, bool isInput, uint32_t index, const char* deviceId, int32_t side);
// Whether AssignHw could use this device for that slot (listed, or room in the list).
bool CanAssignHw(const PanelModel& model, bool isInput, uint32_t index, const char* deviceId);
// A BRIDGE(n) slot on app channel `channel` (0 = Ch 1): INPUTS rows may share one; an OUTPUTS channel
// is taken by one row.
bool CanAssignBridge(const PanelModel& model, bool isInput, uint32_t index, WHASlotType type, int32_t channel);
// What a slot carries, for the Source column: "Microphone · L", "Virtual 2 · R", "Rx3 · Ch1", "Bridge1 · Ch2".
// hwDevices: HwDeviceNames of the slot's direction (nullptr: unknown).
std::string SlotSourceLabel(const WHASlot& slot, bool isInput, const char* const* hwDevices);
// Source menu entries: "WinHookAudio Virtual 1" (GENERAL name prefix), "Rx1  from 192.168.1.50:6980 ·
// PCM 32 · 2 ch" or "(not set up: NETWORK tab)", "Bridge 1  (2 apps connected)" (shared = nullptr: unknown).
std::string VirtualCableLabel(const WHAGeneral& general, int cable);
std::string NetworkStreamLabel(const WHANetworkStream& stream, bool isInput, int index);
std::string BridgeLabel(int bridge, const WHABridgeShared* shared);

// Bridge popup (read-only): each channel the app sees, and the Master row it is routed to. One line
// per row (INPUTS rows may share a channel); a channel no row picks is one unrouted line.
struct BridgeRoute {
  int channel = 0;         // the app's channel, 0 = Ch 1
  int masterSlot = -1;     // Master INPUTS (app outputs) or OUTPUTS (app inputs) row, -1 = not routed
  std::string masterName;  // that row's name as the Master DAW sees it
};
// appOutputs: the app's outputs, summed into Master INPUTS; else its inputs, from Master OUTPUTS.
std::vector<BridgeRoute> BridgeRoutes(const WHASlotTable& table, int bridgeIndex, bool appOutputs);

// GENERAL HW status: requested versus actual. `stats` is the streaming Master's (nullptr: the DAW has
// not started the driver); `saved` is the Slot Table's GENERAL now. HW settings are read when the
// DAW starts the driver, so a Save that changed them shows as pending until the DAW resets it.
enum class HwStatusLevel { Info, Ok, Warning, Error };
struct HwStatusLine {
  HwStatusLevel level;
  std::string text;
};
// savedMore: the Slot Table's more-devices list now (nullptr: not compared).
std::vector<HwStatusLine> HwStatusLines(const WHAMasterStats* stats, const WHAGeneral& saved, const PanelDevices* devices,
                                        const WHAHwMore* savedMore = nullptr);
// Why a HW open failed, in words ("" for an unknown HRESULT).
const char* HwErrorText(int32_t hr);

// Helpers
const char* SlotDisplayName(const WHASlot& slot);  // "- empty -" for SLOT_NONE else name
bool IsLoopbackEditable(const WHASlot& slot);  // true only if VIRTUAL

}  // namespace wha
