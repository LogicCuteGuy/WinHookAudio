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
    if (!MatchesFilter(slots[i], model.filter)) continue;
    if (!MatchesSearch(slots[i], model.search)) continue;
    // Bridge popup filtered to BRIDGE1 only
    if (!model.isMaster && slots[i].type != SLOT_BRIDGE1) continue;
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
    for (uint32_t i = from; i < to; ++i) model.table.masterIn[i] = model.table.masterIn[i + 1];
  } else {
    for (uint32_t i = from; i > to; --i) model.table.masterIn[i] = model.table.masterIn[i - 1];
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
  // Clear loopback if not VIRTUAL
  if (type != SLOT_VIRTUAL) model.table.masterIn[index].loopback = 0;
  return true;
}

}  // namespace wha
