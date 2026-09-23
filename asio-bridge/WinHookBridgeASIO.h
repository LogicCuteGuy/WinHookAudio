#pragma once

// WinHookAudio Bridge ASIO driver — the IASIO a Slave DAW loads (Steinberg ASIO SDK 2.3.4 ABI).
// Vocabulary: Bridge Driver, Shared Bridge, Slot, Master Clock.
// Client inputs = Master OUT slots of type BRIDGE(n) (broadcast to all clients);
// client outputs = Master IN slots of type BRIDGE(n) (summed across up to 4 clients by the Worker).

#include "WHAAsio.h"
#include "WHASlotTable.h"
#include "WHABridgeShared.h"
#include "WHASharedMemory.h"

#include <atomic>
#include <vector>

namespace wha {

class ControlPanelWindow;

class WinHookBridgeASIO : public IASIO {
 public:
  explicit WinHookBridgeASIO(int bridgeIndex);
  virtual ~WinHookBridgeASIO();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

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

  uint64_t clockTicks() const { return clockTicks_.load(); }

 private:
  struct Binding {
    bool isInput;
    long channel;
    float* buffers[2];
  };

  long countBridgeChannels(bool isInput) const;
  const WHASlot* bridgeSlot(bool isInput, long n) const;
  static DWORD WINAPI clockProc(LPVOID self);
  void runClock();
  void clockTick();

  std::atomic<ULONG> refCount_{1};
  int bridgeIndex_ = 0;  // 0..3
  int32_t clientId_ = -1;
  bool initialized_ = false;
  bool running_ = false;
  WHASlotTable* slotTable_ = nullptr;
  HANDLE slotTableMapping_ = nullptr;
  WHABridgeShared* bridgeShared_ = nullptr;
  HANDLE bridgeMapping_ = nullptr;
  HANDLE bridgeTick_ = nullptr;
  long bufferSize_ = 128;
  double sampleRate_ = 48000.0;
  char errorText_[124] = {};
  ASIOCallbacks callbacks_ = {};
  ControlPanelWindow* panel_ = nullptr;  // Popup Type 1 filtered to BRIDGE(n)
  HANDLE tableChanged_ = nullptr;

  bool buffersCreated_ = false;
  std::vector<float> dawBuffers_;
  std::vector<Binding> bindings_;
  HANDLE clockThread_ = nullptr;
  std::atomic<bool> clockStop_{false};
  long bufferIndex_ = 0;
  int32_t clientBuf_ = 0;  // clientIn double-buffer half written last
  std::atomic<uint64_t> samplePosition_{0};
  std::atomic<uint64_t> sampleTimeNs_{0};
  std::atomic<uint64_t> clockTicks_{0};
};

}  // namespace wha
