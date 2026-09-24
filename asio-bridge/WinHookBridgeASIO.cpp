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
  releaseClientPlace();
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
  return BridgeChannelCount(slots, count, want);
}
// The Master slot on channel n, or nullptr when no slot picks it (a silent channel).
const WHASlot* WinHookBridgeASIO::bridgeSlot(bool isInput, long n) const {
  if (!slotTable_ || n < 0 || n >= countBridgeChannels(isInput)) return nullptr;
  const WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
  const WHASlot* slots = isInput ? slotTable_->masterOut : slotTable_->masterIn;
  const uint32_t count = isInput ? slotTable_->masterOutCount : slotTable_->masterInCount;
  const int i = FindBridgeSlot(slots, count, want, static_cast<int>(n));
  return i >= 0 ? &slots[i] : nullptr;
}

namespace {

// A place's owner still runs. A process ID that cannot be opened for another reason than "no such
// process" (access denied) counts as running, so a live app never loses its place.
bool ProcessRunning(DWORD pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return GetLastError() != ERROR_INVALID_PARAMETER;
  const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return running;
}

}  // namespace

// One of the Bridge's 4 places for this driver instance (other processes claim at the same time: the
// compare-exchange decides). A free place first; when all 4 are held, one whose app has exited (crashed,
// killed) is taken over.
bool WinHookBridgeASIO::claimClientPlace() {
  const auto me = static_cast<LONG>(GetCurrentProcessId());
  auto* owner = reinterpret_cast<volatile LONG*>(bridgeShared_->owner);
  for (int32_t i = 0; i < static_cast<int32_t>(kBridgeClients); ++i)
    if (InterlockedCompareExchange(&owner[i], me, 0) == 0) { clientId_ = i; break; }
  for (int32_t i = 0; clientId_ < 0 && i < static_cast<int32_t>(kBridgeClients); ++i) {
    const LONG held = owner[i];
    if (held != 0 && held != me && !ProcessRunning(static_cast<DWORD>(held)) &&
        InterlockedCompareExchange(&owner[i], me, held) == held)
      clientId_ = i;
  }
  if (clientId_ < 0) return false;
  bridgeShared_->ready[clientId_] = 0;  // nothing from a previous holder in the next sum
  return true;
}

// Gives the place back when the app closes the driver (Release, or a DAW switching drivers).
void WinHookBridgeASIO::releaseClientPlace() {
  if (!bridgeShared_ || clientId_ < 0) return;
  bridgeShared_->ready[clientId_] = 0;
  auto* owner = reinterpret_cast<volatile LONG*>(bridgeShared_->owner);
  InterlockedCompareExchange(&owner[clientId_], 0, static_cast<LONG>(GetCurrentProcessId()));
  clientId_ = -1;
}

std::string WinHookBridgeASIO::dawView() const {
  const WHASlotTable& t = *slotTable_;
  std::string view = std::to_string(t.general.sampleRate) + "/" + std::to_string(t.general.asioBuffer);
  for (const bool isInput : {true, false}) {
    const long n = countBridgeChannels(isInput);
    view += "|" + std::to_string(n);
    const WHASlot* slots = isInput ? t.masterOut : t.masterIn;
    const uint32_t count = isInput ? t.masterOutCount : t.masterInCount;
    for (long ch = 0; ch < n; ++ch) {
      const WHASlot* slot = bridgeSlot(isInput, ch);
      char name[kNameLen] = "-";
      if (slot) DawChannelName(slots, count, static_cast<uint32_t>(slot - slots), !isInput, nullptr, name);
      view += ";";
      view += name;
    }
  }
  return view;
}

void WinHookBridgeASIO::snapshotDawView() {
  const uint32_t version = slotTable_->version;
  std::string view = dawView();
  std::lock_guard<std::mutex> lock(dawViewMutex_);
  dawView_ = std::move(view);
  resetPending_ = false;
  seenVersion_ = version;
}

void WinHookBridgeASIO::requestReset() {
  {
    std::lock_guard<std::mutex> lock(dawViewMutex_);
    if (resetPending_) return;
    resetPending_ = true;
  }
  if (callbacks_.asioMessage && callbacks_.asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1)
    callbacks_.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
}

// A Save (Master panel or a Bridge popup) bumps the Slot Table version. The Bridge has no TableChanged
// of its own (that event is the Master's, auto-reset), so the clock thread compares versions each period
// and rebuilds the view only when one moved.
void WinHookBridgeASIO::checkTableChanged() {
  const uint32_t version = slotTable_->version;
  if (version == seenVersion_.load()) return;
  seenVersion_ = version;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(dawViewMutex_);
    changed = !resetPending_ && dawView() != dawView_;
  }
  if (changed) requestReset();  // e.g. a Bridge channel added in the Master panel: FL re-queries getChannels
}

ASIOBool WinHookBridgeASIO::init(void* sysHandle) {
  (void)sysHandle;
  if (initialized_) return ASIOTrue;
  // The Master owns the Slot Table, the Bridge regions and the clock: without an open Master there is
  // nothing to bridge to. Only the Master creates Master_Tick, so it tells whether one is open now.
  constexpr const char* kNoMaster = "Open WinHookAudio Master in your main DAW first";
  HANDLE masterTick = OpenEventA(SYNCHRONIZE, FALSE, shm::kMasterTickName + 7);
  if (!masterTick) {
    std::snprintf(errorText_, sizeof(errorText_), "%s", kNoMaster);
    return ASIOFalse;
  }
  CloseHandle(masterTick);
  slotTableMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kSlotTableName + 7);
  if (slotTableMapping_)
    slotTable_ = static_cast<WHASlotTable*>(MapViewOfFile(slotTableMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize));
  bridgeMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kBridgeSharedNames[bridgeIndex_] + 7);
  if (bridgeMapping_)
    bridgeShared_ = static_cast<WHABridgeShared*>(MapViewOfFile(bridgeMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kBridgeSharedSize));
  if (!slotTable_ || !bridgeShared_ || slotTable_->version == 0) {
    std::snprintf(errorText_, sizeof(errorText_), "%s", kNoMaster);
    return ASIOFalse;  // the destructor releases what was opened
  }
  if (!claimClientPlace()) {
    std::snprintf(errorText_, sizeof(errorText_), "Bridge %d is full (4 apps) - use another Bridge", bridgeIndex_ + 1);
    return ASIOFalse;
  }
  bridgeTick_ = CreateEventA(nullptr, FALSE, FALSE, shm::kBridgeTickNames[bridgeIndex_][clientId_] + 7);
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
  snapshotDawView();
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
  if (info->channel < 0 || info->channel >= countBridgeChannels(isInput)) return ASE_InvalidParameter;
  const WHASlot* slot = bridgeSlot(isInput, info->channel);
  info->isActive = ASIOFalse;
  for (const Binding& b : bindings_)
    if (b.isInput == isInput && b.channel == info->channel) info->isActive = ASIOTrue;
  info->channelGroup = 0;
  info->type = ASIOSTFloat32LSB;
  // Client inputs come from Master OUT slots, client outputs feed Master IN slots.
  const WHASlot* slots = isInput ? slotTable_->masterOut : slotTable_->masterIn;
  const uint32_t count = isInput ? slotTable_->masterOutCount : slotTable_->masterInCount;
  char name[kNameLen];
  if (slot) DawChannelName(slots, count, static_cast<uint32_t>(slot - slots), !isInput, nullptr, name);  // Bridge slots only
  else std::snprintf(name, sizeof(name), "Bridge%d Ch%ld", bridgeIndex_ + 1, info->channel + 1);  // no slot: silent
  TruncateCopy(info->name, sizeof(info->name), name);
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
    checkTableChanged();  // after the period: a changed view only asks the DAW to reset
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
    if (reset) requestReset();
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
