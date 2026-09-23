#pragma once

// WHAControlPanelView — ImGui drawing of the panel model (Datasheet FINAL v10 §8, ADR 0008).
// Vocabulary: Slot, Slot Table, Control Panel, Loopback, Shared Bridge, Network Stream.
// Pure ImGui: no Win32/DX11, so it renders headless in panel-view-test. Hosted by WHAControlPanelWindow.

#include <cstdint>
#include <string>
#include "WHAControlPanelUI.h"

namespace wha {

struct PanelViewState {
  int bridgeIndex = -1;  // >= 0: Bridge popup, INPUTS/OUTPUTS locked to BRIDGE(n+1)
  PanelFilter inFilter = PanelFilter::All;
  PanelFilter outFilter = PanelFilter::All;
  char inSearch[64] = {};
  char outSearch[64] = {};
  bool dirty = false;       // edit copy differs from the table it was opened from; set by the host
  std::string status;       // last Save/Import/Export result, shown in the footer
  int rowsDrawnIn = 0;      // rows the ListClipper actually submitted last frame
  int rowsDrawnOut = 0;
  int requestTab = -1;      // PanelTab to select on the next frame, then reset to -1
  int activeTab = -1;       // PanelTab drawn last frame
};

enum PanelTab : int { kTabInputs = 0, kTabOutputs, kTabNetwork, kTabGeneral, kTabAbout };

struct PanelViewResult {
  bool save = false;
  bool revert = false;
  bool exportSlots = false;
  bool importSlots = false;
  bool close = false;
};

// Draws one frame of the full-window panel. Mutations go through the WHAControlPanelUI model
// functions and are applied after the list is drawn, so indices stay stable while iterating.
PanelViewResult DrawControlPanel(PanelModel& edit, PanelViewState& state, WHABridgeShared* bridges[4]);

// Dark #1E1E1E theme from the datasheet.
void ApplyPanelStyle();

// IM_ASSERT failures since process start (see wha_imconfig.h).
int ImGuiAssertCount();

}  // namespace wha
