#pragma once

// WHAControlPanelUI — panel model for INPUTS 512 (11).
// Vocabulary: Slot, Slot Table, Control Panel, Loopback, Shared Bridge.
// No ImGui required for offline tests — pure model.

#include <cstdint>
#include <string>
#include "WHASlotTable.h"

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

// Helpers
const char* SlotDisplayName(const WHASlot& slot);  // "- empty -" for SLOT_NONE else name
bool IsLoopbackEditable(const WHASlot& slot);  // true only if VIRTUAL

}  // namespace wha
