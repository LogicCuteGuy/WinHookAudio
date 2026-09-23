#pragma once

#include "ASIOStub.h"
#include "WHASlotTable.h"
#include "WHABridgeShared.h"
#include "WHASharedMemory.h"

namespace wha {

class ControlPanelWindow;

class WinHookBridgeASIO : public IASIO {
 public:
  explicit WinHookBridgeASIO(int bridgeIndex);
  virtual ~WinHookBridgeASIO();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

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

 private:
  int countBridgeChannels() const;
  const WHASlot* bridgeSlot(bool isInput, int32_t n) const;

  ULONG refCount_ = 1;
  int bridgeIndex_ = 0;  // 0..3
  int32_t clientId_ = -1;
  bool initialized_ = false;
  WHASlotTable* slotTable_ = nullptr;
  HANDLE slotTableMapping_ = nullptr;
  WHABridgeShared* bridgeShared_ = nullptr;
  HANDLE bridgeMapping_ = nullptr;
  HANDLE bridgeTick_ = nullptr;
  int32_t bufferSize_ = 128;
  char errorText_[128] = {};
  ASIOCallbacks callbacks_ = {};
  ControlPanelWindow* panel_ = nullptr;  // Popup Type 1 filtered to BRIDGE(n)
  HANDLE tableChanged_ = nullptr;
};

}  // namespace wha
