#pragma once

// WHAControlPanelWindow — Popup Type 1 (Datasheet FINAL v10 §8, ADR 0008):
// controlPanel() -> own thread -> Win32 + DX11 + ImGui, 1180x720 TopMost, dark #1E1E1E.
// Vocabulary: Control Panel, Slot Table, Master Driver, Bridge Driver.
// The panel edits a copy; Save commits it (CommitPanelSave). Closing without Save discards the copy.

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>

#include "WHAControlPanelUI.h"
#include "WHAControlPanelView.h"

namespace wha {

struct ControlPanelHost {
  WHASlotTable* table = nullptr;             // live Slot Table (SHM view)
  HANDLE tableChanged = nullptr;             // signalled after Save so the Worker reloads
  WHABridgeShared* bridges[4] = {};          // client counts for ABOUT/badges (may be null)
  bool isMaster = true;
  int bridgeIndex = -1;                      // Bridge popup: 0..3, locks lists to BRIDGE(n+1)
  std::function<void(bool resetRequested)> onSaved;  // driver: asioMessage(kAsioResetRequest)
  std::string slotsJsonPath;                 // empty = %ProgramData%\WinHookAudio\slots.json
  // Master: the streaming driver's counters for GENERAL "actual" (false = not started). Any thread.
  std::function<bool(WHAMasterStats&)> readStats;
  // Offline smoke test hooks
  bool hidden = false;
  int autoCloseAfterFrames = 0;              // > 0: render N frames, then close
  std::function<void(PanelViewResult&, int frame)> testFrameHook;  // inject clicks (tests only)
};

// Save: validate -> *table = edit (version++) -> slots.json -> FlushViewOfFile -> SetEvent(TableChanged)
// -> onSaved(reset). Reset is requested when the Master Clock or the DAW-visible channel list changed,
// because the DAW must re-query getChannels/getChannelInfo/getBufferSize.
bool CommitPanelSave(PanelModel& edit, const ControlPanelHost& host, std::string* status);

// Expands %ProgramData% etc.; returns false if the path cannot be expanded.
bool ExpandSlotsJsonPath(const std::string& path, std::string* expanded);

class ControlPanelWindow {
 public:
  ControlPanelWindow() = default;
  ~ControlPanelWindow();
  ControlPanelWindow(const ControlPanelWindow&) = delete;
  ControlPanelWindow& operator=(const ControlPanelWindow&) = delete;

  // Starts the popup thread, or brings the open popup to the front.
  bool Open(const ControlPanelHost& host);
  // Asks the popup to close (discarding unsaved edits) and joins its thread;
  // cancels an open Export/Import dialog so a DAW unloading the driver never waits on the user.
  void Close();
  bool IsOpen() const;
  // Waits for the popup thread to end on its own (e.g. autoCloseAfterFrames).
  bool WaitClosed(DWORD timeoutMs);

  int FramesRendered() const { return frames_.load(); }
  bool UsedWarp() const { return usedWarp_.load(); }
  std::string LastError() const;

 private:
  static DWORD WINAPI ThreadProc(void* self);
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void Run();

  ControlPanelHost host_;
  HANDLE thread_ = nullptr;
  DWORD threadId_ = 0;
  std::atomic<HWND> hwnd_{nullptr};
  std::atomic<bool> quit_{false};
  std::atomic<int> frames_{0};
  std::atomic<bool> usedWarp_{false};
  std::string lastError_;  // written by the popup thread before it exits
  struct Gpu;
  Gpu* gpu_ = nullptr;     // owned by the popup thread
};

}  // namespace wha
