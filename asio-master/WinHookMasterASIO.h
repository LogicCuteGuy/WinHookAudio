#pragma once

// WinHookAudio Master ASIO — minimal IASIO for 08.
// Vocabulary: Master Driver, Slot, Slot Table, Master Clock.

#include "ASIOStub.h"
#include "WHASlotTable.h"
#include "WHASharedMemory.h"
#include "WHABridgeShared.h"

#include <atomic>
#include <mutex>

namespace wha {

class ControlPanelWindow;

class WinHookMasterASIO : public IASIO {
 public:
  WinHookMasterASIO();
  virtual ~WinHookMasterASIO();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IASIO
  ASIOError init(void* sysHandle) override;
  void getDriverName(char* name) override;
  int32_t getDriverVersion() override;
  void getErrorMessage(char* text) override;
  ASIOError start() override;
  ASIOError stop() override;
  ASIOError getChannels(int32_t* numInputChannels, int32_t* numOutputChannels) override;
  ASIOError getLatencies(int32_t* inputLatency, int32_t* outputLatency) override;
  ASIOError getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) override;
  ASIOError canSampleRate(double sampleRate) override;
  ASIOError getSampleRate(double* sampleRate) override;
  ASIOError setSampleRate(double sampleRate) override;
  ASIOError getClockSources(int64_t* clocks, int32_t* numSources) override;
  ASIOError setClockSource(int32_t reference) override;
  ASIOError getSamplePosition(int64_t* sPos, int64_t* tPos) override;
  ASIOError getChannelInfo(ASIOChannelInfo* info) override;
  ASIOError createBuffers(ASIOChannelInfo* infos, int32_t numChannels, int32_t bufferSize, ASIOCallbacks* callbacks) override;
  ASIOError disposeBuffers() override;
  ASIOError controlPanel() override;
  ASIOError future(int32_t selector, void* opt) override;
  ASIOError outputReady() override;

  // For testing: simulate hostCallback(ASIOResetRequest) trigger
  bool resetRequested() const { return resetRequested_; }
  void clearResetRequest() { resetRequested_ = false; }

 private:
  void requestReset();  // hostCallback(ASIOResetRequest): DAW re-queries channels/clock
  void snapshotDawView();   // record what the DAW has just queried
  void onTableChanged();    // Worker thread: reset if the DAW-visible table drifted from the snapshot

  ULONG refCount_ = 1;
  bool initialized_ = false;
  bool running_ = false;
  WHASlotTable* slotTable_ = nullptr;
  HANDLE slotTableMapping_ = nullptr;
  HANDLE masterAudioMapping_ = nullptr;
  float* masterAudio_ = nullptr;  // 16MB ping-pong
  HANDLE bridgeMappings_[4] = {};
  WHABridgeShared* bridgeShared_[4] = {};
  HANDLE masterTick_ = nullptr;
  HANDLE tableChanged_ = nullptr;
  HANDLE bridgeTicks_[4][4] = {};
  ASIOCallbacks callbacks_ = {};
  int32_t bufferSize_ = 128;
  std::atomic<bool> resetRequested_{false};  // set from DAW thread or Control Panel thread
  char errorText_[128] = {};
  class MasterHolder* holder_ = nullptr;
  ControlPanelWindow* panel_ = nullptr;  // Popup Type 1, created on first controlPanel()
  std::mutex dawViewMutex_;
  WHASlotTable dawView_{};      // table as last seen by the DAW (getChannels)
  bool resetPending_ = false;   // sent ASIOResetRequest, DAW has not re-queried yet
};

}  // namespace wha
