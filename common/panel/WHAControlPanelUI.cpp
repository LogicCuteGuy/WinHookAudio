#include "WHAControlPanelUI.h"
#include "WHASlotsJson.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace wha {

bool MatchesFilter(const WHASlot& slot, PanelFilter filter) {
  if (filter == PanelFilter::All) return true;
  if (filter == PanelFilter::HW) return slot.type == SLOT_HW;
  if (filter == PanelFilter::Virtual) return slot.type == SLOT_VIRTUAL;
  if (filter == PanelFilter::Network) return slot.type == SLOT_NETWORK;
  if (filter == PanelFilter::Bridge1) return slot.type == SLOT_BRIDGE1;
  if (filter == PanelFilter::Bridge2) return slot.type == SLOT_BRIDGE2;
  if (filter == PanelFilter::Bridge3) return slot.type == SLOT_BRIDGE3;
  if (filter == PanelFilter::Bridge4) return slot.type == SLOT_BRIDGE4;
  return false;
}

bool MatchesSearch(const WHASlot& slot, const std::string& search) {
  if (search.empty()) return true;
  std::string name(slot.name);
  std::string lowerName, lowerSearch;
  lowerName.resize(name.size());
  lowerSearch.resize(search.size());
  std::transform(name.begin(), name.end(), lowerName.begin(), [](unsigned char c){ return static_cast<char>(::tolower(c)); });
  std::transform(search.begin(), search.end(), lowerSearch.begin(), [](unsigned char c){ return static_cast<char>(::tolower(c)); });
  return lowerName.find(lowerSearch) != std::string::npos;
}

int FilteredCount(const PanelModel& model, bool isInput) {
  int count = 0;
  uint32_t n = isInput ? model.table.masterInCount : model.table.masterOutCount;
  const WHASlot* slots = isInput ? model.table.masterIn : model.table.masterOut;
  for (uint32_t i = 0; i < n; ++i) {
    // Bridge popup: always filtered to BRIDGE1, ignore Filter setting
    if (!model.isMaster) {
      if (slots[i].type != SLOT_BRIDGE1) continue;
      if (!MatchesSearch(slots[i], model.search)) continue;
      ++count;
      continue;
    }
    if (!MatchesFilter(slots[i], model.filter)) continue;
    if (!MatchesSearch(slots[i], model.search)) continue;
    ++count;
  }
  return count;
}

const char* SlotDisplayName(const WHASlot& slot) {
  if (slot.type == SLOT_NONE) return "- empty -";
  return slot.name;
}

bool IsLoopbackEditable(const WHASlot& slot) {
  return slot.type == SLOT_VIRTUAL;
}

bool AddInput(PanelModel& model) {
  if (model.table.masterInCount >= kMax) return false;
  uint32_t idx = model.table.masterInCount;
  model.table.masterIn[idx].type = SLOT_NONE;
  model.table.masterIn[idx].enabled = 0;
  model.table.masterIn[idx].loopback = 0;
  model.table.masterIn[idx].srcChannel = 0;
  model.table.masterIn[idx].streamId = 0;
  TruncateCopy(model.table.masterIn[idx].name, kNameLen, "- empty -");
  model.table.masterInCount++;
  return true;
}

bool InsertEmptyAbove(PanelModel& model, uint32_t index) {
  if (model.table.masterInCount >= kMax) return false;
  if (index > model.table.masterInCount) return false;
  for (uint32_t i = model.table.masterInCount; i > index; --i) {
    model.table.masterIn[i] = model.table.masterIn[i - 1];
  }
  model.table.masterIn[index].type = SLOT_NONE;
  model.table.masterIn[index].enabled = 0;
  model.table.masterIn[index].loopback = 0;
  model.table.masterIn[index].srcChannel = 0;
  model.table.masterIn[index].streamId = 0;
  TruncateCopy(model.table.masterIn[index].name, kNameLen, "- empty -");
  model.table.masterInCount++;
  return true;
}

bool InsertEmptyBelow(PanelModel& model, uint32_t index) {
  if (index >= model.table.masterInCount) return false;
  return InsertEmptyAbove(model, index + 1);
}

bool DuplicateInput(PanelModel& model, uint32_t index) {
  if (model.table.masterInCount >= kMax) return false;
  if (index >= model.table.masterInCount) return false;
  for (uint32_t i = model.table.masterInCount; i > index + 1; --i) {
    model.table.masterIn[i] = model.table.masterIn[i - 1];
  }
  model.table.masterIn[index + 1] = model.table.masterIn[index];
  model.table.masterInCount++;
  return true;
}

bool DeleteInput(PanelModel& model, uint32_t index) {
  if (index >= model.table.masterInCount) return false;
  if (model.table.masterInCount <= 1) return false;  // keep at least 1
  for (uint32_t i = index; i + 1 < model.table.masterInCount; ++i) {
    model.table.masterIn[i] = model.table.masterIn[i + 1];
  }
  model.table.masterInCount--;
  // Clear last
  model.table.masterIn[model.table.masterInCount] = WHASlot{};
  return true;
}

bool MoveInput(PanelModel& model, uint32_t from, uint32_t to) {
  if (from >= model.table.masterInCount || to >= model.table.masterInCount) return false;
  if (from == to) return true;
  WHASlot tmp = model.table.masterIn[from];
  if (from < to) {
    std::memmove(&model.table.masterIn[from], &model.table.masterIn[from + 1], (to - from) * sizeof(WHASlot));
  } else {
    std::memmove(&model.table.masterIn[to + 1], &model.table.masterIn[to], (from - to) * sizeof(WHASlot));
  }
  model.table.masterIn[to] = tmp;
  return true;
}

bool SetInputLoopback(PanelModel& model, uint32_t index, bool loopback) {
  if (index >= model.table.masterInCount) return false;
  if (loopback && !IsLoopbackEditable(model.table.masterIn[index])) return false;
  model.table.masterIn[index].loopback = loopback ? 1 : 0;
  return true;
}

bool SetInputEnabled(PanelModel& model, uint32_t index, bool enabled) {
  if (index >= model.table.masterInCount) return false;
  model.table.masterIn[index].enabled = enabled ? 1 : 0;
  return true;
}

bool SetInputName(PanelModel& model, uint32_t index, const char* name) {
  if (index >= model.table.masterInCount) return false;
  TruncateCopy(model.table.masterIn[index].name, kNameLen, name);
  return true;
}

namespace {

// A HW slot's source is L or R of the stereo device: a channel left over from another type is reset.
void ClampHwSource(WHASlot& s) {
  if (s.type == SLOT_HW && (s.srcChannel < 0 || s.srcChannel >= kHwSlotChannels)) s.srcChannel = 0;
}

bool IsValidSource(WHASlotType type, int32_t srcChannel, int32_t streamId) {
  switch (type) {
    case SLOT_HW: return srcChannel >= 0 && srcChannel < kHwSlotChannels;
    case SLOT_NETWORK:
      return streamId >= 0 && streamId < static_cast<int32_t>(kNetStreams) && srcChannel >= 0 &&
             srcChannel < static_cast<int32_t>(kMaxPcmChannels);
    case SLOT_VIRTUAL: return srcChannel >= 0 && srcChannel < 2 * kVirtualSlotCables;  // cable * 2 + side
    default: return srcChannel >= 0 && srcChannel < static_cast<int32_t>(kMax);
  }
}

}  // namespace

bool SetInputType(PanelModel& model, uint32_t index, WHASlotType type) {
  if (index >= model.table.masterInCount) return false;
  if (type > SLOT_BRIDGE4) return false;
  model.table.masterIn[index].type = type;
  if (type != SLOT_VIRTUAL) model.table.masterIn[index].loopback = 0;
  ClampHwSource(model.table.masterIn[index]);
  return true;
}

bool SetInputSource(PanelModel& model, uint32_t index, int32_t srcChannel, int32_t streamId) {
  if (index >= model.table.masterInCount) return false;
  WHASlot& s = model.table.masterIn[index];
  if (!IsValidSource(s.type, srcChannel, streamId)) return false;
  s.srcChannel = srcChannel;
  s.streamId = streamId;
  return true;
}

// OUTPUTS — independent indices
bool AddOutput(PanelModel& model) {
  if (model.table.masterOutCount >= kMax) return false;
  uint32_t idx = model.table.masterOutCount;
  model.table.masterOut[idx].type = SLOT_NONE;
  model.table.masterOut[idx].enabled = 0;
  model.table.masterOut[idx].loopback = 0;
  model.table.masterOut[idx].srcChannel = 0;
  model.table.masterOut[idx].streamId = 0;
  TruncateCopy(model.table.masterOut[idx].name, kNameLen, "- empty -");
  model.table.masterOutCount++;
  return true;
}
bool InsertEmptyAboveOutput(PanelModel& model, uint32_t index) {
  if (model.table.masterOutCount >= kMax) return false;
  if (index > model.table.masterOutCount) return false;
  for (uint32_t i = model.table.masterOutCount; i > index; --i) model.table.masterOut[i] = model.table.masterOut[i - 1];
  model.table.masterOut[index].type = SLOT_NONE;
  model.table.masterOut[index].enabled = 0;
  model.table.masterOut[index].loopback = 0;
  model.table.masterOut[index].srcChannel = 0;
  model.table.masterOut[index].streamId = 0;
  TruncateCopy(model.table.masterOut[index].name, kNameLen, "- empty -");
  model.table.masterOutCount++;
  return true;
}
bool InsertEmptyBelowOutput(PanelModel& model, uint32_t index) {
  if (index >= model.table.masterOutCount) return false;
  return InsertEmptyAboveOutput(model, index + 1);
}
bool DuplicateOutput(PanelModel& model, uint32_t index) {
  if (model.table.masterOutCount >= kMax) return false;
  if (index >= model.table.masterOutCount) return false;
  for (uint32_t i = model.table.masterOutCount; i > index + 1; --i) model.table.masterOut[i] = model.table.masterOut[i - 1];
  model.table.masterOut[index + 1] = model.table.masterOut[index];
  model.table.masterOutCount++;
  return true;
}
bool DeleteOutput(PanelModel& model, uint32_t index) {
  if (index >= model.table.masterOutCount) return false;
  if (model.table.masterOutCount <= 1) return false;
  for (uint32_t i = index; i + 1 < model.table.masterOutCount; ++i) model.table.masterOut[i] = model.table.masterOut[i + 1];
  model.table.masterOutCount--;
  model.table.masterOut[model.table.masterOutCount] = WHASlot{};
  return true;
}
bool MoveOutput(PanelModel& model, uint32_t from, uint32_t to) {
  if (from >= model.table.masterOutCount || to >= model.table.masterOutCount) return false;
  if (from == to) return true;
  WHASlot tmp = model.table.masterOut[from];
  if (from < to) std::memmove(&model.table.masterOut[from], &model.table.masterOut[from + 1], (to - from) * sizeof(WHASlot));
  else std::memmove(&model.table.masterOut[to + 1], &model.table.masterOut[to], (from - to) * sizeof(WHASlot));
  model.table.masterOut[to] = tmp;
  return true;
}
bool SetOutputLoopback(PanelModel& model, uint32_t index, bool loopback) {
  if (index >= model.table.masterOutCount) return false;
  if (loopback && !IsLoopbackEditable(model.table.masterOut[index])) return false;
  model.table.masterOut[index].loopback = loopback ? 1 : 0;
  return true;
}
bool SetOutputEnabled(PanelModel& model, uint32_t index, bool enabled) {
  if (index >= model.table.masterOutCount) return false;
  model.table.masterOut[index].enabled = enabled ? 1 : 0;
  return true;
}
bool SetOutputName(PanelModel& model, uint32_t index, const char* name) {
  if (index >= model.table.masterOutCount) return false;
  TruncateCopy(model.table.masterOut[index].name, kNameLen, name);
  return true;
}
bool SetOutputType(PanelModel& model, uint32_t index, WHASlotType type) {
  if (index >= model.table.masterOutCount) return false;
  if (type > SLOT_BRIDGE4) return false;
  model.table.masterOut[index].type = type;
  if (type != SLOT_VIRTUAL) model.table.masterOut[index].loopback = 0;
  ClampHwSource(model.table.masterOut[index]);
  return true;
}

bool SetOutputSource(PanelModel& model, uint32_t index, int32_t srcChannel, int32_t streamId) {
  if (index >= model.table.masterOutCount) return false;
  WHASlot& s = model.table.masterOut[index];
  if (!IsValidSource(s.type, srcChannel, streamId)) return false;
  s.srcChannel = srcChannel;
  s.streamId = streamId;
  return true;
}

// GENERAL Per-Thing
bool IsGeneralReadOnly(const PanelModel& model) { return !model.isMaster; }
bool SetMasterClock(PanelModel& model, uint32_t sampleRate, uint32_t asioBuffer) {
  if (IsGeneralReadOnly(model)) return false;
  if (!IsValidMasterClock(sampleRate, asioBuffer)) return false;
  model.table.general.sampleRate = sampleRate;
  model.table.general.asioBuffer = asioBuffer;
  return true;
}
namespace {
bool SetEndpointId(PanelModel& model, char* field, const char* id) {
  if (IsGeneralReadOnly(model) || !id || std::strlen(id) >= kEndpointIdLen) return false;
  TruncateCopy(field, kEndpointIdLen, id);
  return true;
}
}  // namespace

bool SetHwRenderDevice(PanelModel& model, const char* id) { return SetEndpointId(model, model.table.general.hwRenderId, id); }
bool SetHwCaptureDevice(PanelModel& model, const char* id) { return SetEndpointId(model, model.table.general.hwCaptureId, id); }

const char* EndpointName(const std::vector<PanelEndpoint>& list, const char* id) {
  for (const PanelEndpoint& e : list)
    if (e.id == id) return e.name.c_str();
  return nullptr;
}

const char* HwErrorText(int32_t hr) {
  switch (static_cast<HRESULT>(hr)) {
    case AUDCLNT_E_DEVICE_IN_USE: return "in use by another application (exclusive)";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "exclusive mode not allowed (Sound settings > device Properties > Advanced)";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return "no stereo float32/24-bit/16-bit format at this sample rate";
    case AUDCLNT_E_INVALID_DEVICE_PERIOD: return "period not supported by the device";
    case AUDCLNT_E_BUFFER_SIZE_ERROR: return "buffer size not supported by the device";
    case AUDCLNT_E_DEVICE_INVALIDATED: return "device removed or disabled";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "device could not be opened";
    case AUDCLNT_E_CPUUSAGE_EXCEEDED: return "audio engine CPU limit exceeded";
    case HRESULT_FROM_WIN32(ERROR_NOT_FOUND): return "device not found (unplugged or disabled?)";
    case E_ACCESSDENIED: return "access denied";
    case E_FAIL: return "failed to start";
    default: return "";
  }
}

namespace {

const char* HwFormatText(int32_t format) {
  switch (format) {
    case HW_FORMAT_FLOAT32: return "float32";
    case HW_FORMAT_PCM24IN32: return "24-bit";
    case HW_FORMAT_PCM16: return "16-bit";
    default: return "?";
  }
}

double FramesMs(int32_t frames, int32_t rate) { return rate > 0 ? 1000.0 * frames / rate : 0.0; }

// "Speakers" for a listed ID, else the ID itself (the device may be gone from the list).
std::string DeviceLabel(const std::vector<PanelEndpoint>* list, const char* id) {
  const char* name = list ? EndpointName(*list, id) : nullptr;
  return name ? name : (id && *id ? id : "device");
}

// "period 128 frames (2.67 ms; requested 64: device minimum)"
std::string PeriodText(int32_t actual, int32_t requested, int32_t rate) {
  char buf[128];
  const int n = std::snprintf(buf, sizeof(buf), "period %d frames (%.2f ms", actual, FramesMs(actual, rate));
  std::string s(buf, n > 0 ? static_cast<size_t>(n) : 0);
  if (requested == 0) s += "; Auto";
  else if (actual > requested) s += "; requested " + std::to_string(requested) + ": device minimum";
  else if (actual != requested) s += "; requested " + std::to_string(requested) + ": device-aligned";
  return s + ")";
}

HwStatusLine HwFailedLine(const char* direction, int32_t hr, const char* consequence) {
  char buf[256];
  const char* why = HwErrorText(hr);
  std::snprintf(buf, sizeof(buf), "%s: FAILED - %s (0x%08X). %s", direction, *why ? why : "open error",
                static_cast<unsigned>(hr), consequence);
  return {HwStatusLevel::Error, buf};
}

}  // namespace

std::vector<HwStatusLine> HwStatusLines(const WHAMasterStats* st, const WHAGeneral& saved, const PanelDevices* devices) {
  std::vector<HwStatusLine> lines;
  if (!st) {
    lines.push_back({HwStatusLevel::Info, "Not streaming: HW devices open when the DAW starts the driver."});
    return lines;
  }
  const int32_t rate = st->sampleRate;
  char buf[512];

  if (st->hwOpen) {
    std::snprintf(buf, sizeof(buf), "Output: %s - exclusive %s, %s, latency %.1f ms",
                  DeviceLabel(devices ? &devices->render : nullptr, st->hwRenderId).c_str(), HwFormatText(st->hwFormat),
                  PeriodText(st->hwPeriod, st->hwRequestedPeriod, rate).c_str(), FramesMs(st->hwLatency, rate));
    lines.push_back({HwStatusLevel::Ok, buf});
    if (st->hwChunk > 0) {
      std::snprintf(buf, sizeof(buf), "Output device plays in %d-frame chunks (%.1f ms): the DAW is still called evenly",
                    st->hwChunk, FramesMs(st->hwChunk, rate));
      lines.push_back({HwStatusLevel::Info, buf});
    }
    if (st->hwUnderruns || st->hwDrops) {
      std::snprintf(buf, sizeof(buf), "Output: %llu underruns, %llu dropped blocks (audible gaps; a larger HW buffer helps)",
                    static_cast<unsigned long long>(st->hwUnderruns), static_cast<unsigned long long>(st->hwDrops));
      lines.push_back({HwStatusLevel::Warning, buf});
    }
  } else if (st->hwLastError) {
    lines.push_back(HwFailedLine("Output", st->hwLastError, "HW OUT slots are silent."));
  } else {
    lines.push_back({HwStatusLevel::Info, "Output: not open (no HW slot when the DAW started)."});
  }

  if (st->hwInOpen) {
    std::snprintf(buf, sizeof(buf), "Input: %s - exclusive %s, %s, latency %.1f ms, clock %+.1f ppm%s",
                  DeviceLabel(devices ? &devices->capture : nullptr, st->hwCaptureId).c_str(),
                  HwFormatText(st->hwInFormat), PeriodText(st->hwInPeriod, st->hwRequestedPeriod, rate).c_str(),
                  FramesMs(st->hwInLatency, rate), st->hwInDriftPpmMilli / 1000.0,
                  st->hwInDriftEngaged ? " (resampling)" : "");
    lines.push_back({HwStatusLevel::Ok, buf});
    // Every way HW input audio can break, since the DAW started: the backlog ran empty (a gap, then
    // the buffer grows), overflowed (frames thrown away), the device flagged a gap, or the Worker
    // missed Master Clock ticks (their frames dropped to stay in step).
    if (st->hwInStarved || st->hwInTrims || st->hwInGlitches || st->hwInSkipped) {
      std::snprintf(buf, sizeof(buf),
                    "Input dropouts: %llu ran empty (buffer grew %llu times), %llu overflowed, %llu device gaps, "
                    "%.1f ms skipped",
                    static_cast<unsigned long long>(st->hwInStarved), static_cast<unsigned long long>(st->hwInGrowths),
                    static_cast<unsigned long long>(st->hwInTrims), static_cast<unsigned long long>(st->hwInGlitches),
                    FramesMs(static_cast<int32_t>(st->hwInSkipped), rate));
      lines.push_back({HwStatusLevel::Warning, buf});
    }
  } else if (st->hwInLastError) {
    lines.push_back(HwFailedLine("Input", st->hwInLastError, "HW IN slots are silent."));
  } else {
    lines.push_back({HwStatusLevel::Info, "Input: not open (no HW IN slot when the DAW started)."});
  }

  if (st->workerOverruns) {
    std::snprintf(buf, sizeof(buf), "Worker late %llu times (the PC could not keep up; a larger ASIO buffer helps)",
                  static_cast<unsigned long long>(st->workerOverruns));
    lines.push_back({HwStatusLevel::Warning, buf});
  }
  lines.push_back({HwStatusLevel::Info, st->clockSource == CLOCK_HARDWARE
                                            ? "Master Clock: HW output (the device paces the DAW)."
                                            : "Master Clock: internal timer (no HW output open)."});

  if (st->hwRequestValid &&
      (static_cast<uint32_t>(st->hwRequestedPeriod) != saved.hwBuffer ||
       std::strncmp(st->hwRequestedRenderId, saved.hwRenderId, kEndpointIdLen) != 0 ||
       std::strncmp(st->hwRequestedCaptureId, saved.hwCaptureId, kEndpointIdLen) != 0))
    lines.push_back({HwStatusLevel::Warning,
                     "Saved HW settings are not in use yet: they apply when the DAW resets the driver "
                     "(restart audio in the DAW if it does not)."});
  return lines;
}

const char* HwDeviceName(const PanelDevices* devices, const WHAGeneral& general, bool isInput) {
  if (!devices) return nullptr;
  const char* id = isInput ? general.hwCaptureId : general.hwRenderId;
  if (!id[0]) id = (isInput ? devices->defaultCaptureId : devices->defaultRenderId).c_str();
  return id[0] ? EndpointName(isInput ? devices->capture : devices->render, id) : nullptr;
}

bool AssignSource(PanelModel& model, bool isInput, uint32_t index, WHASlotType type, int32_t srcChannel, int32_t streamId) {
  const uint32_t count = isInput ? model.table.masterInCount : model.table.masterOutCount;
  if (index >= count || type > SLOT_BRIDGE4 || !IsValidSource(type, srcChannel, streamId)) return false;
  if (isInput) return SetInputType(model, index, type) && SetInputSource(model, index, srcChannel, streamId);
  return SetOutputType(model, index, type) && SetOutputSource(model, index, srcChannel, streamId);
}

bool AssignHw(PanelModel& model, bool isInput, uint32_t index, const char* deviceId, int32_t side) {
  const uint32_t count = isInput ? model.table.masterInCount : model.table.masterOutCount;
  if (index >= count || !deviceId || std::strlen(deviceId) >= kEndpointIdLen || IsGeneralReadOnly(model)) return false;
  if (!IsValidSource(SLOT_HW, side, 0)) return false;
  const bool deviceSet = isInput ? SetHwCaptureDevice(model, deviceId) : SetHwRenderDevice(model, deviceId);
  return deviceSet && AssignSource(model, isInput, index, SLOT_HW, side, 0);
}

std::string SlotSourceLabel(const WHASlot& slot, bool isInput, const char* hwDevice) {
  char buf[96];
  switch (slot.type) {
    case SLOT_NONE: return "- empty -";
    case SLOT_HW: {
      char device[kNameLen];
      ShortDeviceName(hwDevice, device, sizeof(device));
      const char side = slot.srcChannel == 0 ? 'L' : (slot.srcChannel == 1 ? 'R' : '?');
      std::snprintf(buf, sizeof(buf), "%s \xC2\xB7 %c", device[0] ? device : (isInput ? "HW In" : "HW Out"), side);
      return buf;
    }
    case SLOT_VIRTUAL:
      std::snprintf(buf, sizeof(buf), "Virtual %d \xC2\xB7 %c", VirtualCableOf(slot) + 1, VirtualSideOf(slot) ? 'R' : 'L');
      return buf;
    case SLOT_NETWORK:
      std::snprintf(buf, sizeof(buf), "%s%d \xC2\xB7 Ch%d", isInput ? "Rx" : "Tx", slot.streamId + 1, slot.srcChannel + 1);
      return buf;
    default:
      std::snprintf(buf, sizeof(buf), "Bridge%d", static_cast<int>(slot.type - SLOT_BRIDGE1) + 1);
      return buf;
  }
}

std::string VirtualCableLabel(const WHAGeneral& general, int cable) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s %d", general.virtualName[0] ? general.virtualName : "Virtual", cable + 1);
  return buf;
}

std::string NetworkStreamLabel(const WHANetworkStream& stream, bool isInput, int index) {
  char buf[96];
  if (!stream.ip[0]) {
    std::snprintf(buf, sizeof(buf), "%s%d  (not set up: NETWORK tab)", isInput ? "Rx" : "Tx", index + 1);
    return buf;
  }
  const char* codec = stream.codec == WHA_PCM_I16 ? "PCM 16" : (stream.codec == WHA_VORBIS ? "Vorbis" : "PCM 32");
  std::snprintf(buf, sizeof(buf), "%s%d  %s %s:%u \xC2\xB7 %s \xC2\xB7 %u ch", isInput ? "Rx" : "Tx", index + 1,
                isInput ? "from" : "to", stream.ip, static_cast<unsigned>(stream.port), codec, stream.channels);
  return buf;
}

std::string BridgeLabel(int bridge, const WHABridgeShared* shared) {
  char buf[64];
  if (!shared) {
    std::snprintf(buf, sizeof(buf), "Bridge %d", bridge + 1);
  } else {
    const int apps = shared->clientCount;
    if (apps <= 0) std::snprintf(buf, sizeof(buf), "Bridge %d  (no app connected)", bridge + 1);
    else std::snprintf(buf, sizeof(buf), "Bridge %d  (%d app%s connected)", bridge + 1, apps, apps == 1 ? "" : "s");
  }
  return buf;
}

bool SetHwBuffer(PanelModel& model, uint32_t frames) {
  if (IsGeneralReadOnly(model)) return false;
  if (frames != 0 && frames != 64 && frames != 128 && frames != 256 && frames != 512 && frames != 1024) return false;
  model.table.general.hwBuffer = frames;  // 0 = Auto (device minimum period)
  return true;
}
bool SetVirtualBuffer(PanelModel& model, uint32_t frames) {
  if (IsGeneralReadOnly(model)) return false;
  if (frames != 64 && frames != 128 && frames != 256 && frames != 512 && frames != 1024) return false;
  model.table.general.virtualBuffer = frames;
  return true;
}
bool SetBridgeBuffer(PanelModel& model, int bridgeIndex, uint32_t frames) {
  if (IsGeneralReadOnly(model)) return false;
  if (bridgeIndex < 0 || bridgeIndex >= 4) return false;
  if (frames != 64 && frames != 128 && frames != 256 && frames != 512 && frames != 1024) return false;
  model.table.general.bridgeBuffer[bridgeIndex] = frames;
  return true;
}
bool SetNetworkPcmBuffer(PanelModel& model, uint32_t frames) {
  if (IsGeneralReadOnly(model)) return false;
  if (frames != 64 && frames != 128 && frames != 256 && frames != 512 && frames != 1024) return false;
  model.table.general.networkPcmBuffer = frames;
  return true;
}
bool SetNetworkVorbisBuffer(PanelModel& model, uint32_t frames) {
  if (IsGeneralReadOnly(model)) return false;
  if (frames != 64 && frames != 128 && frames != 256 && frames != 512 && frames != 1024) return false;
  model.table.general.networkVorbisBuffer = frames;
  return true;
}
bool SetJitterPcm(PanelModel& model, uint32_t ms) {
  if (IsGeneralReadOnly(model)) return false;
  model.table.general.jitterPcm = ms;
  return true;
}
bool SetJitterVorbis(PanelModel& model, uint32_t ms) {
  if (IsGeneralReadOnly(model)) return false;
  model.table.general.jitterVorbis = ms;
  return true;
}

// ABOUT + Save contract (13)
AboutInfo GetAboutInfo(const PanelModel& model, WHABridgeShared* bridges[4]) {
  AboutInfo info;
  info.version = "1.0.0";
  info.sysRunning = false;
  info.clsidCount = 5;
  info.slotsJsonPath = "%ProgramData%\\WinHookAudio\\slots.json";
  (void)model;
  for (int i = 0; i < 4; ++i) {
    char buf[32];
    if (bridges && bridges[i]) {
      int count = bridges[i]->clientCount;
      if (count > 4) count = 4;
      if (count < 0) count = 0;
      std::snprintf(buf, sizeof(buf), "Bridge%d: %d/4", i + 1, count);
    } else {
      // No Shared Bridge mapped in this process: client count is unknown (slot count is not client count).
      std::snprintf(buf, sizeof(buf), "Bridge%d: -/4", i + 1);
    }
    info.bridgeClients[i] = buf;
  }
  return info;
}

bool SavePanel(PanelModel& editCopy, WHASlotTable* pTable, std::string* jsonOut, bool* resetRequested) {
  if (!pTable) return false;
  std::string err;
  if (!ValidateSlots(editCopy.table, &err)) return false;
  bool masterClockChanged = (editCopy.table.general.sampleRate != pTable->general.sampleRate ||
                             editCopy.table.general.asioBuffer != pTable->general.asioBuffer);
  editCopy.table.version = pTable->version + 1;
  *pTable = editCopy.table;
  std::string json = SerializeSlots(*pTable);
  if (jsonOut) *jsonOut = json;
  if (resetRequested) *resetRequested = masterClockChanged;
  return true;
}

bool ExportSlots(const WHASlotTable& table, const std::string& path) {
  std::string json = SerializeSlots(table);
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
  std::fwrite(json.c_str(), 1, json.size(), f);
  std::fclose(f);
  return true;
}

bool ImportSlots(WHASlotTable& table, const std::string& path, std::string* error) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) { if (error) *error = "open failed"; return false; }
  std::fseek(f, 0, SEEK_END);
  long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string json;
  json.resize(len);
  std::fread(json.data(), 1, len, f);
  std::fclose(f);
  return DeserializeSlots(json, table, error);
}

bool ResetToDefault(PanelModel& model) {
  model.table = WHASlotTable{};  // WHAGeneral in-class initializers give 64/256/128/512/1024/20/50/8
  model.table.masterInCount = 2;
  model.table.masterIn[0].type = SLOT_HW; model.table.masterIn[0].enabled = 1;
  TruncateCopy(model.table.masterIn[0].name, kNameLen, "Mic 1");
  model.table.masterIn[1].type = SLOT_NONE; model.table.masterIn[1].enabled = 0;
  TruncateCopy(model.table.masterIn[1].name, kNameLen, "- empty -");
  model.table.masterOutCount = 2;
  model.table.masterOut[0].type = SLOT_HW; model.table.masterOut[0].enabled = 1;
  TruncateCopy(model.table.masterOut[0].name, kNameLen, "Main L");
  model.table.masterOut[1].type = SLOT_NONE; model.table.masterOut[1].enabled = 0;
  TruncateCopy(model.table.masterOut[1].name, kNameLen, "- empty -");
  model.table.version = 1;
  return true;
}

// GENERAL Virtual Cables (18)
bool SetVirtualCableCount(PanelModel& model, uint32_t count) {
  if (IsGeneralReadOnly(model)) return false;
  if (count != 8 && count != 64) return false;
  model.table.general.virtualCables = count;
  return true;
}
bool SetVirtualCableName(PanelModel& model, const char* name) {
  if (IsGeneralReadOnly(model)) return false;
  if (!name) return false;
  TruncateCopy(model.table.general.virtualName, 32, name);
  return true;
}
uint32_t GetVirtualCableCount(const PanelModel& model) { return model.table.general.virtualCables; }
std::string GetVirtualCableName(const PanelModel& model) { return std::string(model.table.general.virtualName); }

// NETWORK 8 tab (16)
bool SetNetworkTx(PanelModel& model, uint32_t index, const WHANetworkStream& stream) {
  if (index >= kNetStreams) return false;
  if (!IsValidCodec(stream.codec)) return false;
  if (!IsValidNetworkChannels(stream.codec, stream.channels)) return false;
  if (stream.codec == WHA_VORBIS && (stream.quality < 0.1f || stream.quality > 1.0f)) return false;
  if (stream.port == 0) return false;
  model.table.netTx[index] = stream;
  return true;
}
bool SetNetworkRx(PanelModel& model, uint32_t index, const WHANetworkStream& stream) {
  if (index >= kNetStreams) return false;
  if (!IsValidCodec(stream.codec)) return false;
  if (!IsValidNetworkChannels(stream.codec, stream.channels)) return false;
  if (stream.codec == WHA_VORBIS && (stream.quality < 0.1f || stream.quality > 1.0f)) return false;
  if (stream.port == 0) return false;
  model.table.netRx[index] = stream;
  return true;
}
WHANetworkStream GetNetworkTx(const PanelModel& model, uint32_t index) {
  if (index >= kNetStreams) return WHANetworkStream{};
  return model.table.netTx[index];
}
WHANetworkStream GetNetworkRx(const PanelModel& model, uint32_t index) {
  if (index >= kNetStreams) return WHANetworkStream{};
  return model.table.netRx[index];
}


}  // namespace wha
