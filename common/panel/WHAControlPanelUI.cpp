#include "WHAControlPanelUI.h"
#include <cstring>
#include <algorithm>

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

}  // namespace wha
