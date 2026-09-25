#pragma once

// WinHookAudio Master ASIO driver — the IASIO the Master DAW loads (Steinberg ASIO SDK 2.3.4 ABI).
// Vocabulary: Master Driver, Slot, Slot Table, Master Clock, Worker.
// The Master Clock (sample rate + ASIO buffer) is set in the Control Panel; the DAW gets exactly that
// buffer, and may change the rate (setSampleRate, any of kSampleRates).
// A Master Clock thread calls bufferSwitch every period and hands audio to/from the Worker via SHM.

#include "WHAAsio.h"
#include "WHASlotTable.h"
#include "WHASharedMemory.h"
#include "WHABridgeShared.h"
#include "WHAMasterStats.h"
#include "HwClockPacer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

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
  ASIOBool init(void* sysHandle) override;
  void getDriverName(char* name) override;
  long getDriverVersion() override;
  void getErrorMessage(char* string) override;
  ASIOError start() override;
  ASIOError stop() override;
  ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
  ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
  ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
  ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
  ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
  ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
  ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
  ASIOError setClockSource(long reference) override;
  ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
  ASIOError getChannelInfo(ASIOChannelInfo* info) override;
  ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                          ASIOCallbacks* callbacks) override;
  ASIOError disposeBuffers() override;
  ASIOError controlPanel() override;
  ASIOError future(long selector, void* opt) override;
  ASIOError outputReady() override;

  // For testing: whether a reset was requested from the DAW
  bool resetRequested() const { return resetRequested_; }
  void clearResetRequest() { resetRequested_ = false; }
  uint64_t clockTicks() const { return clockTicks_.load(); }
  uint64_t clockOverruns() const { return clockOverruns_.load(); }
  uint64_t workerOverruns() const { return workerOverruns_.load(); }
  // WHAGetMasterStats: the instance between start() and stop(), and its counters. stats() is safe
  // from any thread (the Control Panel polls it); false while not started.
  static WinHookMasterASIO* streaming();
  bool stats(WHAMasterStats* out) const;

 private:
  struct Binding {  // one DAW channel's double buffer, created by createBuffers
    bool isInput;
    long channel;
    float* buffers[2];
  };

  void requestReset();      // hostCallback(ASIOResetRequest): DAW re-queries channels/clock
  void snapshotDawView();   // record what the DAW has just queried
  void onTableChanged();    // Worker thread: reset if the DAW-visible table drifted from the snapshot
  static DWORD WINAPI clockProc(LPVOID self);
  void runClock();
  void clockTick();
  class KsCapture* reportedCapture() const;  // the HW input the DAW's input latency describes

  std::atomic<ULONG> refCount_{1};
  bool initialized_ = false;
  bool claimed_ = false;  // this instance holds (a share of) MasterClaim
  bool running_ = false;
  WHASlotTable* slotTable_ = nullptr;
  HANDLE slotTableMapping_ = nullptr;
  HANDLE masterAudioMapping_ = nullptr;
  float* masterAudio_ = nullptr;  // OUT slot n at n*4096, IN slot n at (512+n)*4096
  HANDLE bridgeMappings_[4] = {};
  WHABridgeShared* bridgeShared_[4] = {};
  HANDLE masterTick_ = nullptr;
  HANDLE tableChanged_ = nullptr;
  HANDLE bridgeTicks_[4][4] = {};
  ASIOCallbacks callbacks_ = {};
  long bufferSize_ = 128;
  double sampleRate_ = 48000.0;
  std::atomic<bool> resetRequested_{false};  // set from DAW thread or Control Panel thread
  char errorText_[124] = {};
  class MasterHolder* holder_ = nullptr;
  mutable std::mutex holderMutex_;  // holder_ lifetime vs stats() on another thread
  ControlPanelWindow* panel_ = nullptr;  // Popup Type 1, created on first controlPanel()
  std::mutex dawViewMutex_;
  WHASlotTable dawView_{};      // table as last seen by the DAW (getChannels)
  bool resetPending_ = false;   // sent ASIOResetRequest, DAW has not re-queried yet
  std::string hwInNames_[kHwDevices];  // HW device friendly names at getChannels (DAW thread), for channel names
  std::string hwOutNames_[kHwDevices];

  // Buffers + Master Clock
  bool buffersCreated_ = false;
  std::vector<float> dawBuffers_;
  std::vector<Binding> bindings_;
  HANDLE clockThread_ = nullptr;
  std::atomic<bool> clockStop_{false};
  long bufferIndex_ = 0;
  std::atomic<uint64_t> samplePosition_{0};  // frames at the last bufferSwitch
  std::atomic<uint64_t> sampleTimeNs_{0};    // system time of the last bufferSwitch
  std::atomic<uint64_t> clockTicks_{0};
  std::atomic<uint64_t> clockOverruns_{0};   // internal timeline resyncs (stall > kMaxCatchUpPeriods)
  std::atomic<uint64_t> workerOverruns_{0};  // ticks the Worker had not routed within one period
  std::atomic<int32_t> clockSource_{CLOCK_INTERNAL};  // WHAClockSource of the last tick
  std::atomic<uint64_t> hwFillSum_{0};    // device fill summed over HW Master Clock ticks
  std::atomic<uint64_t> hwFillTicks_{0};
  // HW Master Clock pacing (clock thread), see HwClockPacer.
  HwClockPacer pacer_;
  const class KsAudio* pacedHw_ = nullptr;      // the device pacer_ was reset for
  std::atomic<int32_t> hwOutReportFill_{-1};
  std::atomic<bool> hwClockSettled_{false};     // the HW Master Clock's warm-up is over (MasterHolder::setClockSettled)    // device fill the output latency is reported from (-1: targetFill)
  std::atomic<bool> hwOutLatencyChanged_{false};  // the DAW must re-query getLatencies
  std::atomic<int32_t> hwChunk_{0};
  std::atomic<uint64_t> hwHurries_{0};
};

}  // namespace wha
