#include "WHAControlPanelUI.h"
#include "WHASlotsJson.h"
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

bool SetInputType(PanelModel& model, uint32_t index, WHASlotType type) {
  if (index >= model.table.masterInCount) return false;
  if (type > SLOT_BRIDGE4) return false;
  model.table.masterIn[index].type = type;
  if (type != SLOT_VIRTUAL) model.table.masterIn[index].loopback = 0;
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
bool SetHwBuffer(PanelModel& model, uint32_t frames) {
  if (IsGeneralReadOnly(model)) return false;
  if (frames != 64 && frames != 128 && frames != 256 && frames != 512 && frames != 1024) return false;
  model.table.general.hwBuffer = frames;
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
