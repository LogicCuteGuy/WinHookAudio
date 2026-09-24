// panel-gui-test — offline checks for the ImGui Control Panel (ADR 0008).
// Headless ImGui frames (no window) + CommitPanelSave contract + hidden DX11 popup smoke run.
// No DAW, no audio: stream_verified stays false.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <cstring>
#include <string>

#include "WHAControlPanelView.h"
#include "WHAControlPanelWindow.h"
#include "WHASlotsFile.h"
#include "WHASlotsJson.h"
#include "imgui.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

bool WaitFor(const std::function<bool()>& pred, DWORD ms) {
  const ULONGLONG end = GetTickCount64() + ms;
  while (GetTickCount64() < end) {
    if (pred()) return true;
    Sleep(10);
  }
  return pred();
}

WHASlotTable MakeTable512() {
  PanelModel m;
  ResetToDefault(m);
  WHASlotTable t = m.table;
  t.masterInCount = kMax;
  t.masterOutCount = kMax;
  for (uint32_t i = 0; i < kMax; ++i) {
    WHASlot& in = t.masterIn[i];
    in = WHASlot{};
    in.type = i % 10 == 3 ? SLOT_BRIDGE1 : SLOT_HW;
    in.enabled = 1;
    in.srcChannel = static_cast<int32_t>(i);
    char name[kNameLen];
    std::snprintf(name, sizeof(name), "In %u", i + 1);
    SetSlotName(in, name);
    t.masterOut[i] = in;
    std::snprintf(name, sizeof(name), "Out %u", i + 1);
    SetSlotName(t.masterOut[i], name);
  }
  return t;
}

// One headless frame at 1180x720 (fonts built, no renderer backend needed).
PanelViewResult Frame(PanelModel& edit, PanelViewState& state) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1180, 720);
  io.DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  const PanelViewResult r = DrawControlPanel(edit, state, nullptr);
  ImGui::Render();
  return r;
}

void HeadlessView() {
  ImGuiContext* ctx = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  unsigned char* pixels = nullptr;
  int w = 0, h = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  ApplyPanelStyle();

  PanelModel edit;
  edit.table = MakeTable512();
  PanelViewState state;
  Frame(edit, state);
  Frame(edit, state);
  std::printf("INPUTS 512: %d rows drawn\n", state.rowsDrawnIn);
  check("INPUTS default tab", state.activeTab == kTabInputs);
  check("INPUTS ListClipper draws a page, not 512", state.rowsDrawnIn > 0 && state.rowsDrawnIn <= 40);

  strcpy_s(state.inSearch, "In 50");
  Frame(edit, state);
  int expected = 0;
  for (uint32_t i = 0; i < kMax; ++i) expected += MatchesSearch(edit.table.masterIn[i], "In 50") ? 1 : 0;
  std::printf("Search 'In 50': %d rows drawn, %d expected\n", state.rowsDrawnIn, expected);
  check("INPUTS search filters rows", state.rowsDrawnIn == expected);
  state.inSearch[0] = '\0';

  // GENERAL with enumerated devices: a known saved device, and one no longer present.
  PanelDevices devices;
  devices.render.push_back({"{render-a}", "Speakers (Test)"});
  devices.capture.push_back({"{capture-a}", "Microphone (Test)"});
  state.devices = &devices;
  SetHwRenderDevice(edit, "{render-a}");
  SetHwCaptureDevice(edit, "{unplugged}");
  const char* tabNames[] = {"INPUTS", "OUTPUTS", "NETWORK", "GENERAL", "ABOUT"};
  for (int tab = kTabInputs; tab <= kTabAbout; ++tab) {
    state.requestTab = tab;
    Frame(edit, state);
    Frame(edit, state);
    char name[48];
    std::snprintf(name, sizeof(name), "%s tab renders", tabNames[tab]);
    check(name, state.activeTab == tab);
  }
  state.requestTab = kTabOutputs;
  Frame(edit, state);
  Frame(edit, state);
  check("OUTPUTS ListClipper draws a page, not 512", state.rowsDrawnOut > 0 && state.rowsDrawnOut <= 40);

  const WHASlotTable before = edit.table;
  for (int i = 0; i < 3; ++i) Frame(edit, state);
  check("Rendering alone does not edit the table", std::memcmp(&before, &edit.table, sizeof(WHASlotTable)) == 0);

  // Bridge popup: locked to BRIDGE1, GENERAL read-only
  PanelModel bridge;
  bridge.table = MakeTable512();
  bridge.isMaster = false;
  PanelViewState bstate;
  bstate.bridgeIndex = 0;
  bstate.requestTab = kTabInputs;  // same headless context: the tab bar remembers the last tab
  Frame(bridge, bstate);
  Frame(bridge, bstate);
  int bridgeSlots = 0;
  for (uint32_t i = 0; i < kMax; ++i) bridgeSlots += bridge.table.masterIn[i].type == SLOT_BRIDGE1 ? 1 : 0;
  std::printf("Bridge1 popup: filter %u, %d rows drawn of %d BRIDGE1 slots\n", static_cast<unsigned>(bstate.inFilter),
              bstate.rowsDrawnIn, bridgeSlots);
  check("Bridge popup locked to BRIDGE1", bstate.inFilter == PanelFilter::Bridge1 && bstate.rowsDrawnIn > 0 &&
                                              bstate.rowsDrawnIn <= bridgeSlots);
  bstate.requestTab = kTabGeneral;
  Frame(bridge, bstate);
  Frame(bridge, bstate);
  check("Bridge GENERAL renders read-only", bstate.activeTab == kTabGeneral && !SetHwBuffer(bridge, 128));

  // Bridge popup view (read-only): renders a 512-slot table, open and closed Master, no edits.
  WHABridgeShared* shared = new WHABridgeShared();
  shared->owner[0] = 100;
  bool bridgeClose = false;
  for (int i = 0; i < 3; ++i) {
    io.DisplaySize = ImVec2(560, 540);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    bridgeClose |= DrawBridgePanel(bridge.table, 0, i == 0 ? shared : nullptr, i != 1).close;
    ImGui::Render();
  }
  delete shared;
  check("Bridge popup view renders without closing", !bridgeClose);

  ImGui::DestroyContext(ctx);
}

void SaveContract(const std::string& dir) {
  WHASlotTable live = MakeTable512();
  live.version = 7;
  HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  int saves = 0;
  bool lastReset = false;
  ControlPanelHost host;
  host.table = &live;
  host.tableChanged = changed;
  host.configDir = dir;
  host.onSaved = [&](bool reset) {
    ++saves;
    lastReset = reset;
  };

  PanelModel edit;
  edit.table = live;
  SetInputName(edit, 0, "SM58 Mic");
  std::string status;
  check("Save rename commits", CommitPanelSave(edit, host, &status));
  std::printf("  status: %s\n", status.c_str());
  check("Save bumps version", live.version == 8 && std::strcmp(live.masterIn[0].name, "SM58 Mic") == 0);
  check("Save signals TableChanged", WaitForSingleObject(changed, 0) == WAIT_OBJECT_0);
  check("Save rename requests DAW reset", saves == 1 && lastReset);
  WHASlotTable reread{};
  std::string err;
  SetEnvironmentVariableA("WINHOOKAUDIO_CONFIG_DIR", dir.c_str());
  const bool loaded = LoadConfigFiles(reread, &err);
  SetEnvironmentVariableA("WINHOOKAUDIO_CONFIG_DIR", nullptr);
  if (!loaded) std::printf("  load error: %s\n", err.c_str());
  check("routes.yml + settings.yml written and load back",
        loaded && std::strcmp(reread.masterIn[0].name, "SM58 Mic") == 0 && reread.version == 8 &&
            reread.masterInCount == live.masterInCount && reread.general.sampleRate == live.general.sampleRate);

  edit.table = live;
  SetVirtualBuffer(edit, 512);
  check("Save Per-Thing buffer commits", CommitPanelSave(edit, host, &status));
  check("Per-Thing buffer does not reset DAW", saves == 2 && !lastReset && live.general.virtualBuffer == 512);
  // The HW period does: the Worker opens the device with it at start, and it sets the latencies.
  edit.table = live;
  SetHwBuffer(edit, live.general.hwBuffer == 256 ? 512 : 256);
  check("Save HW buffer commits", CommitPanelSave(edit, host, &status));
  check("HW buffer resets DAW", saves == 3 && lastReset);
  edit.table = live;

  edit.table = live;
  SetMasterClock(edit, 96000, 256);
  check("Save Master Clock commits", CommitPanelSave(edit, host, &status));
  check("Master Clock change resets DAW", saves == 4 && lastReset);

  WaitForSingleObject(changed, 0);  // consume the Master Clock save's signal
  edit.table = live;
  edit.table.masterInCount = 0;
  const WHASlotTable beforeBad = live;
  check("Invalid edit rejected", !CommitPanelSave(edit, host, &status));
  std::printf("  status: %s\n", status.c_str());
  check("Rejected Save leaves live table and event untouched",
        std::memcmp(&beforeBad, &live, sizeof(WHASlotTable)) == 0 && saves == 4 &&
            WaitForSingleObject(changed, 0) == WAIT_TIMEOUT);
  CloseHandle(changed);
}

// Closing the Master panel with unsaved edits asks first: Cancel keeps it open, No drops the edits,
// Yes saves them. A clean panel closes without asking.
void CloseAsks(const std::string& dir) {
  WHASlotTable live = MakeTable512();
  const std::string before = live.masterIn[0].name;
  ControlPanelHost host;
  host.table = &live;
  host.hidden = true;
  host.configDir = dir;
  std::atomic<int> asks{0};
  std::atomic<int> firstAnswer{IDCANCEL}, laterAnswer{IDNO};
  host.askSaveOnClose = [&] { return ++asks == 1 ? firstAnswer.load() : laterAnswer.load(); };
  ControlPanelWindow popup;

  host.testFrameHook = [](PanelViewResult& r, int frame) {
    if (frame == 3) r.close = true;
  };
  check("clean panel: Close closes without asking", popup.Open(host) && popup.WaitClosed(5000) && asks == 0);

  // Renamed at frame 2, Close at frame 4 (Cancel) and again at frame 30 (No).
  host.testEditHook = [](PanelModel& edit, int frame) {
    if (frame == 2) SetInputName(edit, 0, "Unsaved Name");
  };
  host.testFrameHook = [](PanelViewResult& r, int frame) {
    if (frame == 4 || frame == 30) r.close = true;
  };
  check("unsaved panel opens", popup.Open(host));
  WaitFor([&] { return popup.FramesRendered() >= 15; }, 5000);
  check("unsaved Close -> asked, Cancel keeps the panel open", asks == 1 && popup.IsOpen());
  check("second Close -> No closes, edits dropped",
        popup.WaitClosed(5000) && asks == 2 && before == live.masterIn[0].name);

  // Yes saves, then closes.
  asks = 0;
  firstAnswer = IDYES;
  const uint32_t version = live.version;
  check("unsaved panel opens again", popup.Open(host));
  check("Close -> Yes saves and closes", popup.WaitClosed(5000) && asks == 1 &&
                                             std::strcmp(live.masterIn[0].name, "Unsaved Name") == 0 &&
                                             live.version == version + 1);
}

void PopupSmoke() {
  WHASlotTable live = MakeTable512();
  ControlPanelHost host;
  host.table = &live;
  host.hidden = true;
  host.autoCloseAfterFrames = 5;
  ControlPanelWindow popup;
  check("Popup opens", popup.Open(host));
  check("Popup closes after 5 frames", popup.WaitClosed(15000));
  std::printf("Popup: %d frames, %s, error '%s'\n", popup.FramesRendered(), popup.UsedWarp() ? "WARP" : "hardware",
              popup.LastError().c_str());
  check("Popup rendered DX11 frames", popup.FramesRendered() == 5 && popup.LastError().empty());

  host.autoCloseAfterFrames = 0;  // stays open until Close()
  check("Popup reopens", popup.Open(host) && popup.IsOpen());
  check("Second Open while open is a no-op", popup.Open(host));
  Sleep(200);
  popup.Close();
  check("Close joins popup thread", !popup.IsOpen() && popup.FramesRendered() > 0);

  // DAW unloads the driver while the Export dialog is open: Close() must cancel it, not wait for the user.
  host.testFrameHook = [](PanelViewResult& r, int frame) {
    if (frame == 3) r.exportRoutes = true;
  };
  check("Popup opens for dialog test", popup.Open(host));
  WaitFor([&] { return popup.FramesRendered() >= 3; }, 5000);
  Sleep(300);
  const int stalled = popup.FramesRendered();
  Sleep(300);
  check("Export dialog blocks the popup loop", popup.FramesRendered() == stalled && popup.IsOpen());
  std::atomic<bool> closed{false};
  std::thread closer([&] {
    popup.Close();
    closed = true;
  });
  const bool done = WaitFor([&] { return closed.load(); }, 5000);
  check("Close cancels the open dialog within 5 s", done);
  if (!done) {
    std::printf("{\"schema_version\":1,\"operation\":\"panel_gui_test\",\"stream_verified\":false,\"pass\":false}\n");
    std::_Exit(1);  // Close() is stuck; don't hang ctest
  }
  closer.join();

  // The Bridge popup: the small read-only view in a real (hidden) DX11 window.
  ControlPanelHost bridgeHost;
  bridgeHost.table = &live;
  bridgeHost.isMaster = false;
  bridgeHost.bridgeIndex = 1;
  bridgeHost.hidden = true;
  bridgeHost.autoCloseAfterFrames = 5;
  ControlPanelWindow bridgePopup;
  check("Bridge popup opens", bridgePopup.Open(bridgeHost));
  check("Bridge popup renders 5 DX11 frames", bridgePopup.WaitClosed(15000) && bridgePopup.FramesRendered() == 5 &&
                                                  bridgePopup.LastError().empty());
}

}  // namespace

int main() {
  // Manual look: WHA_PANEL_GUI_SHOW=1 opens a visible Master popup on a 512-slot table until it is closed.
  char show[8] = {};
  // WHA_PANEL_GUI_SHOW=bridge opens the Bridge 1 popup instead.
  if (GetEnvironmentVariableA("WHA_PANEL_GUI_SHOW", show, sizeof(show)) > 0) {
    WHASlotTable live = MakeTable512();
    ControlPanelHost host;
    host.table = &live;
    host.isMaster = std::strcmp(show, "bridge") != 0;
    host.bridgeIndex = 0;
    host.configDir = "%TEMP%\\wha-panel-gui-show";
    ControlPanelWindow popup;
    popup.Open(host);
    popup.WaitClosed(INFINITE);
    return 0;
  }

  char tmp[MAX_PATH];
  GetTempPathA(sizeof(tmp), tmp);
  const std::string dir = std::string(tmp) + "wha-panel-gui-test";
  CreateDirectoryA(dir.c_str(), nullptr);

  HeadlessView();
  SaveContract(dir);
  CloseAsks(dir);
  PopupSmoke();
  std::printf("ImGui asserts: %d\n", ImGuiAssertCount());
  check("No ImGui asserts", ImGuiAssertCount() == 0);

  std::printf("{\"schema_version\":1,\"operation\":\"panel_gui_test\",\"stream_verified\":false,\"pass\":%s}\n",
              gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
