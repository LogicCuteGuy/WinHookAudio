#include "WinHookBridgeASIO.h"
#if WHA_HAVE_IMGUI
#include "WHAControlPanelWindow.h"
#endif

#include <avrt.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "avrt.lib")

namespace wha {

namespace {

uint64_t NowNs() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(f.QuadPart));
}

}  // namespace

WinHookBridgeASIO::WinHookBridgeASIO(int bridgeIndex) : bridgeIndex_(bridgeIndex) {}
WinHookBridgeASIO::~WinHookBridgeASIO() {
#if WHA_HAVE_IMGUI
  delete panel_;  // joins the popup thread before the Slot Table view goes away
#endif
  stop();
  disposeBuffers();
  if (bridgeShared_ && clientId_ >= 0) bridgeShared_->ready[clientId_] = 0;
  // The client slot itself is not released: WHABridgeShared has no per-slot ownership yet.
  if (tableChanged_) CloseHandle(tableChanged_);
  if (slotTable_) UnmapViewOfFile(slotTable_);
  if (bridgeShared_) UnmapViewOfFile(bridgeShared_);
  if (slotTableMapping_) CloseHandle(slotTableMapping_);
  if (bridgeMapping_) CloseHandle(bridgeMapping_);
  if (bridgeTick_) CloseHandle(bridgeTick_);
}

HRESULT STDMETHODCALLTYPE WinHookBridgeASIO::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  GUID clsid = bridgeIndex_ == 0 ? CLSID_WinHookBridge1 : bridgeIndex_ == 1 ? CLSID_WinHookBridge2 : bridgeIndex_ == 2 ? CLSID_WinHookBridge3 : CLSID_WinHookBridge4;
  if (riid == IID_IUnknown || riid == clsid) { *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK; }
  *ppv = nullptr; return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE WinHookBridgeASIO::AddRef() { return ++refCount_; }
ULONG STDMETHODCALLTYPE WinHookBridgeASIO::Release() { ULONG c = --refCount_; if (c == 0) delete this; return c; }

long WinHookBridgeASIO::countBridgeChannels(bool isInput) const {
  if (!slotTable_) return 0;
  const WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
  // Client inputs come from Master OUT slots, client outputs feed Master IN slots.
  const WHASlot* slots = isInput ? slotTable_->masterOut : slotTable_->masterIn;
  const uint32_t count = isInput ? slotTable_->masterOutCount : slotTable_->masterInCount;
  long n = 0;
  for (uint32_t i = 0; i < count; ++i) if (slots[i].type == want) ++n;
  return n < static_cast<long>(kBridgeChannels) ? n : static_cast<long>(kBridgeChannels);
}
const WHASlot* WinHookBridgeASIO::bridgeSlot(bool isInput, long n) const {
  if (!slotTable_ || n < 0 || n >= countBridgeChannels(isInput)) return nullptr;
  const WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
  const WHASlot* slots = isInput ? slotTable_->masterOut : slotTable_->masterIn;
  const uint32_t count = isInput ? slotTable_->masterOutCount : slotTable_->masterInCount;
  long idx = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (slots[i].type == want && idx++ == n) return &slots[i];
  return nullptr;
}

ASIOBool WinHookBridgeASIO::init(void* sysHandle) {
  (void)sysHandle;
  if (initialized_) return ASIOTrue;
  slotTableMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kSlotTableName + 7);
  if (!slotTableMapping_) {
    // For offline test without Master, create a minimal table
    slotTableMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(shm::kSlotTableSize), shm::kSlotTableName + 7);
    if (!slotTableMapping_) {
      std::snprintf(errorText_, sizeof(errorText_), "Slot Table unavailable %lu", GetLastError());
      return ASIOFalse;
    }
  }
  slotTable_ = static_cast<WHASlotTable*>(MapViewOfFile(slotTableMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize));
  if (!slotTable_) return ASIOFalse;
  // Bridge shared
  bridgeMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kBridgeSharedNames[bridgeIndex_] + 7);
  if (!bridgeMapping_) {
    bridgeMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(shm::kBridgeSharedSize), shm::kBridgeSharedNames[bridgeIndex_] + 7);
    if (!bridgeMapping_) return ASIOFalse;
  }
  bridgeShared_ = static_cast<WHABridgeShared*>(MapViewOfFile(bridgeMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kBridgeSharedSize));
  if (!bridgeShared_) return ASIOFalse;
  // Allocate clientId
  if (!TryAddBridgeClient(*bridgeShared_, &clientId_)) {
    std::snprintf(errorText_, sizeof(errorText_), "Bridge%d full (4/4) - use Bridge2/3/4", bridgeIndex_ + 1);
    return ASIOFalse;
  }
  bridgeTick_ = CreateEventA(nullptr, FALSE, FALSE, shm::kBridgeTickNames[bridgeIndex_][clientId_] + 7);
  if (slotTable_->version == 0) {
    // Offline test: ensure at least one bridge slot
    slotTable_->masterInCount = 1;
    slotTable_->masterIn[0].type = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
    slotTable_->masterIn[0].enabled = 1;
    TruncateCopy(slotTable_->masterIn[0].name, kNameLen, "Bridge Ch1");
    slotTable_->masterOutCount = 1;
    slotTable_->masterOut[0].type = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
    slotTable_->masterOut[0].enabled = 1;
    TruncateCopy(slotTable_->masterOut[0].name, kNameLen, "Bridge Ch1");
    slotTable_->general.sampleRate = kMasterClockRateDefault;
    slotTable_->general.asioBuffer = kMasterClockBufferDefault;
    slotTable_->version = 1;
  }
  bufferSize_ = static_cast<long>(slotTable_->general.asioBuffer);
  sampleRate_ = static_cast<double>(slotTable_->general.sampleRate);
  initialized_ = true;
  return ASIOTrue;
}

void WinHookBridgeASIO::getDriverName(char* name) { std::snprintf(name, 32, "WinHookAudio Bridge %d", bridgeIndex_ + 1); }
long WinHookBridgeASIO::getDriverVersion() { return 0x00010000; }
void WinHookBridgeASIO::getErrorMessage(char* string) { strcpy_s(string, 124, errorText_); }

ASIOError WinHookBridgeASIO::start() {
  if (!initialized_) return ASE_NotPresent;
  if (!buffersCreated_) return ASE_InvalidMode;
  if (running_) return ASE_OK;
  bufferIndex_ = 0;
  samplePosition_ = 0;
  sampleTimeNs_ = NowNs();
  clockStop_ = false;
  clockThread_ = CreateThread(nullptr, 0, clockProc, this, 0, nullptr);
  if (!clockThread_) return ASE_HWMalfunction;
  running_ = true;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::stop() {
  if (clockThread_) {
    clockStop_ = true;
    WaitForSingleObject(clockThread_, INFINITE);
    CloseHandle(clockThread_);
    clockThread_ = nullptr;
  }
  if (bridgeShared_ && clientId_ >= 0) bridgeShared_->ready[clientId_] = 0;  // not in the next sum
  running_ = false;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::getChannels(long* numInputChannels, long* numOutputChannels) {
  if (!initialized_) return ASE_NotPresent;
  if (numInputChannels) *numInputChannels = countBridgeChannels(true);
  if (numOutputChannels) *numOutputChannels = countBridgeChannels(false);
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::getLatencies(long* inputLatency, long* outputLatency) {
  if (!initialized_) return ASE_NotPresent;
  if (inputLatency) *inputLatency = bufferSize_;
  if (outputLatency) *outputLatency = bufferSize_;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  const long size = static_cast<long>(slotTable_->general.asioBuffer);  // Follows Master
  if (minSize) *minSize = size;
  if (maxSize) *maxSize = size;
  if (preferredSize) *preferredSize = size;
  if (granularity) *granularity = 0;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::canSampleRate(ASIOSampleRate sr) {
  if (!slotTable_) return ASE_NotPresent;
  return static_cast<uint32_t>(sr) == slotTable_->general.sampleRate ? ASE_OK : ASE_NoClock;  // Follows Master
}
ASIOError WinHookBridgeASIO::getSampleRate(ASIOSampleRate* sr) {
  if (!sr || !slotTable_) return ASE_InvalidParameter;
  *sr = static_cast<double>(slotTable_->general.sampleRate);
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::setSampleRate(ASIOSampleRate sr) { return sr == 0.0 ? ASE_OK : canSampleRate(sr); }
ASIOError WinHookBridgeASIO::getClockSources(ASIOClockSource* clocks, long* numSources) {
  if (!clocks || !numSources || *numSources < 1) return ASE_InvalidParameter;
  clocks[0] = ASIOClockSource{};
  clocks[0].associatedChannel = -1;
  clocks[0].associatedGroup = -1;
  clocks[0].isCurrentSource = ASIOTrue;
  strcpy_s(clocks[0].name, sizeof(clocks[0].name), "WinHookAudio Master Clock");
  *numSources = 1;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::setClockSource(long reference) { return reference == 0 ? ASE_OK : ASE_InvalidMode; }
ASIOError WinHookBridgeASIO::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
  if (!sPos || !tStamp) return ASE_InvalidParameter;
  if (!running_) return ASE_SPNotAdvancing;
  ToAsio64(samplePosition_.load(), &sPos->hi, &sPos->lo);
  ToAsio64(sampleTimeNs_.load(), &tStamp->hi, &tStamp->lo);
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::getChannelInfo(ASIOChannelInfo* info) {
  if (!info || !slotTable_) return ASE_InvalidParameter;
  const bool isInput = info->isInput != ASIOFalse;
  const WHASlot* slot = bridgeSlot(isInput, info->channel);
  if (!slot) return ASE_InvalidParameter;
  info->isActive = ASIOFalse;
  for (const Binding& b : bindings_)
    if (b.isInput == isInput && b.channel == info->channel) info->isActive = ASIOTrue;
  info->channelGroup = 0;
  info->type = ASIOSTFloat32LSB;
  TruncateCopy(info->name, sizeof(info->name), slot->name);
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                           ASIOCallbacks* callbacks) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  if (running_) return ASE_InvalidMode;
  if (!bufferInfos || !callbacks || !callbacks->bufferSwitch || numChannels <= 0) return ASE_InvalidParameter;
  if (bufferSize != static_cast<long>(slotTable_->general.asioBuffer) || bufferSize > static_cast<long>(kBridgeFrames))
    return ASE_InvalidMode;
  if (buffersCreated_) disposeBuffers();
  for (long i = 0; i < numChannels; ++i)
    if (bufferInfos[i].channelNum < 0 || bufferInfos[i].channelNum >= countBridgeChannels(bufferInfos[i].isInput != ASIOFalse))
      return ASE_InvalidParameter;
  bufferSize_ = bufferSize;
  sampleRate_ = static_cast<double>(slotTable_->general.sampleRate);
  dawBuffers_.assign(static_cast<size_t>(numChannels) * 2 * bufferSize, 0.0f);
  bindings_.clear();
  for (long i = 0; i < numChannels; ++i) {
    float* base = dawBuffers_.data() + static_cast<size_t>(i) * 2 * bufferSize;
    Binding b{bufferInfos[i].isInput != ASIOFalse, bufferInfos[i].channelNum, {base, base + bufferSize}};
    bufferInfos[i].buffers[0] = b.buffers[0];
    bufferInfos[i].buffers[1] = b.buffers[1];
    bindings_.push_back(b);
  }
  callbacks_ = *callbacks;
  buffersCreated_ = true;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::disposeBuffers() {
  if (running_) stop();
  bindings_.clear();
  dawBuffers_.clear();
  dawBuffers_.shrink_to_fit();
  buffersCreated_ = false;
  return ASE_OK;
}

DWORD WINAPI WinHookBridgeASIO::clockProc(LPVOID self) {
  static_cast<WinHookBridgeASIO*>(self)->runClock();
  return 0;
}

// Follows the Master: each Bridge_Tick (sent after the Worker mixed) runs one period. Without a
// running Master the tick never comes, so a period-length timeout keeps the Slave DAW advancing.
void WinHookBridgeASIO::runClock() {
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  const DWORD periodMs = static_cast<DWORD>(bufferSize_ * 1000.0 / sampleRate_) + 1;
  while (!clockStop_.load()) {
    WaitForSingleObject(bridgeTick_, periodMs);
    if (clockStop_.load()) break;
    clockTick();
  }
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void WinHookBridgeASIO::clockTick() {
  const long index = bufferIndex_;
  bufferIndex_ ^= 1;
  const size_t bytes = sizeof(float) * static_cast<size_t>(bufferSize_);
  WHABridgeShared& b = *bridgeShared_;
  for (const Binding& bind : bindings_)  // Master OUT BRIDGE(n) -> client inputs
    if (bind.isInput) std::memcpy(bind.buffers[index], b.clientOut[clientId_][0][bind.channel], bytes);
  sampleTimeNs_ = NowNs();
  callbacks_.bufferSwitch(index, ASIOTrue);
  const int32_t half = clientBuf_ ^ 1;  // write the half the Worker is not reading
  for (const Binding& bind : bindings_)  // client outputs -> summed into Master IN BRIDGE(n)
    if (!bind.isInput) std::memcpy(b.clientIn[clientId_][half][bind.channel], bind.buffers[index], bytes);
  b.activeBuf[clientId_] = half;
  clientBuf_ = half;
  b.ready[clientId_] = 1;
  samplePosition_ += static_cast<uint64_t>(bufferSize_);
  clockTicks_.fetch_add(1);
}

ASIOError WinHookBridgeASIO::controlPanel() {
#if WHA_HAVE_IMGUI
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  if (!panel_) panel_ = new ControlPanelWindow();
  if (!tableChanged_) tableChanged_ = OpenEventA(EVENT_MODIFY_STATE, FALSE, shm::kTableChangedName + 7);  // Master owns it
  ControlPanelHost host;
  host.table = slotTable_;
  host.tableChanged = tableChanged_;
  host.bridges[bridgeIndex_] = bridgeShared_;
  host.isMaster = false;
  host.bridgeIndex = bridgeIndex_;
  host.onSaved = [this](bool reset) {
    if (reset && callbacks_.asioMessage &&
        callbacks_.asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1)
      callbacks_.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
  };
  return panel_->Open(host) ? ASE_OK : ASE_NotPresent;
#else
  return ASE_NotPresent;  // offline build without ImGui: no popup
#endif
}
ASIOError WinHookBridgeASIO::future(long selector, void* opt) {
  (void)selector;
  (void)opt;
  return ASE_InvalidParameter;
}
ASIOError WinHookBridgeASIO::outputReady() { return ASE_NotPresent; }

}  // namespace wha
