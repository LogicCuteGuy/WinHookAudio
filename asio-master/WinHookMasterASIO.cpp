#include "WinHookMasterASIO.h"
#include "MasterHolder.h"
#include "KsAudio.h"
#include "KsCapture.h"
#if WHA_HAVE_IMGUI
#include "WHAControlPanelWindow.h"
#endif

#include <avrt.h>

#include <cmath>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "avrt.lib")

namespace wha {

namespace {

constexpr size_t kSlotFrames = 4096;  // Master audio SHM stride per slot
// Internal timeline: after a stall shorter than this, tick back-to-back to catch up (the average
// rate stays exact); after a longer one, resync instead of bursting.
constexpr double kMaxCatchUpPeriods = 8;

// A block handed over at a HW Master Clock tick is queued behind the target fill, then crosses the device.
long HwOutputLatency(const KsAudio& hw) { return hw.targetFill() + hw.streamLatency(); }
// A captured frame's age when read (the backlog target, from capture timestamps, plus the
// resampler), then one tick in its IN slot.
long HwInputLatency(const KsCapture& hw, long block) { return hw.latency() + block; }

std::atomic<WinHookMasterASIO*> gStreaming{nullptr};

uint64_t NowNs() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(f.QuadPart));
}

}  // namespace

WinHookMasterASIO::WinHookMasterASIO() = default;
WinHookMasterASIO::~WinHookMasterASIO() {
#if WHA_HAVE_IMGUI
  delete panel_;  // joins the popup thread before the Slot Table view goes away
#endif
  stop();
  disposeBuffers();
  if (slotTableMapping_) CloseHandle(slotTableMapping_);
  if (masterAudioMapping_) CloseHandle(masterAudioMapping_);
  for (int i = 0; i < 4; ++i) if (bridgeMappings_[i]) CloseHandle(bridgeMappings_[i]);
  if (masterTick_) CloseHandle(masterTick_);
  if (tableChanged_) CloseHandle(tableChanged_);
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) if (bridgeTicks_[i][j]) CloseHandle(bridgeTicks_[i][j]);
  if (slotTable_) UnmapViewOfFile(slotTable_);
  if (masterAudio_) UnmapViewOfFile(masterAudio_);
  for (int i = 0; i < 4; ++i) if (bridgeShared_[i]) UnmapViewOfFile(bridgeShared_[i]);
}

HRESULT STDMETHODCALLTYPE WinHookMasterASIO::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown || riid == CLSID_WinHookMaster) {  // ASIO hosts pass the CLSID as the IID
    *ppv = static_cast<IASIO*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE WinHookMasterASIO::AddRef() { return ++refCount_; }
ULONG STDMETHODCALLTYPE WinHookMasterASIO::Release() {
  ULONG c = --refCount_;
  if (c == 0) delete this;
  return c;
}

ASIOBool WinHookMasterASIO::init(void* sysHandle) {
  (void)sysHandle;
  if (initialized_) return ASIOTrue;
  // Create SlotTable SHM 80KB
  slotTableMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(shm::kSlotTableSize), shm::kSlotTableName + 7);
  if (!slotTableMapping_) {
    std::snprintf(errorText_, sizeof(errorText_), "CreateFileMapping SlotTable failed %lu", GetLastError());
    return ASIOFalse;
  }
  slotTable_ = static_cast<WHASlotTable*>(MapViewOfFile(slotTableMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize));
  if (!slotTable_) {
    std::snprintf(errorText_, sizeof(errorText_), "MapViewOfFile SlotTable failed %lu", GetLastError());
    return ASIOFalse;
  }
  // Initialize default table if version==0
  if (slotTable_->version == 0) {
    slotTable_->masterInCount = 2;
    slotTable_->masterOutCount = 2;
    slotTable_->masterIn[0].type = SLOT_HW;
    slotTable_->masterIn[0].enabled = 1;
    TruncateCopy(slotTable_->masterIn[0].name, kNameLen, "Mic 1");
    slotTable_->masterIn[1].type = SLOT_NONE;
    slotTable_->masterIn[1].enabled = 0;
    TruncateCopy(slotTable_->masterIn[1].name, kNameLen, "- empty -");
    slotTable_->masterOut[0].type = SLOT_HW;
    slotTable_->masterOut[0].enabled = 1;
    TruncateCopy(slotTable_->masterOut[0].name, kNameLen, "Main L");
    slotTable_->masterOut[1].type = SLOT_NONE;
    slotTable_->masterOut[1].enabled = 0;
    TruncateCopy(slotTable_->masterOut[1].name, kNameLen, "- empty -");
    slotTable_->general.sampleRate = kMasterClockRateDefault;
    slotTable_->general.asioBuffer = kMasterClockBufferDefault;
    slotTable_->version = 1;
  }
  bufferSize_ = static_cast<long>(slotTable_->general.asioBuffer);
  sampleRate_ = static_cast<double>(slotTable_->general.sampleRate);
  snapshotDawView();
  // Master audio 16MB
  masterAudioMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                           static_cast<DWORD>(shm::kMasterAudioSize), shm::kMasterAudioName + 7);
  if (masterAudioMapping_) masterAudio_ = static_cast<float*>(MapViewOfFile(masterAudioMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kMasterAudioSize));
  if (!masterAudio_) {
    std::snprintf(errorText_, sizeof(errorText_), "Master audio SHM failed %lu", GetLastError());
    return ASIOFalse;
  }
  // Bridge shared 4x8MB
  for (int i = 0; i < 4; ++i) {
    bridgeMappings_[i] = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            static_cast<DWORD>(shm::kBridgeSharedSize), shm::kBridgeSharedNames[i] + 7);
    if (bridgeMappings_[i]) bridgeShared_[i] = static_cast<WHABridgeShared*>(MapViewOfFile(bridgeMappings_[i], FILE_MAP_ALL_ACCESS, 0, 0, shm::kBridgeSharedSize));
  }
  masterTick_ = CreateEventA(nullptr, FALSE, FALSE, shm::kMasterTickName + 7);
  tableChanged_ = CreateEventA(nullptr, FALSE, FALSE, shm::kTableChangedName + 7);
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) bridgeTicks_[i][j] = CreateEventA(nullptr, FALSE, FALSE, shm::kBridgeTickNames[i][j] + 7);
  initialized_ = true;
  return ASIOTrue;
}

void WinHookMasterASIO::getDriverName(char* name) { strcpy_s(name, 32, "WinHookAudio Master"); }
long WinHookMasterASIO::getDriverVersion() { return 0x00010000; }
void WinHookMasterASIO::getErrorMessage(char* string) { strcpy_s(string, 124, errorText_); }

ASIOError WinHookMasterASIO::start() {
  if (!initialized_) return ASE_NotPresent;
  if (!buffersCreated_) return ASE_InvalidMode;  // SDK: createBuffers before start
  if (running_) return ASE_OK;
  if (!holder_) {
    holder_ = new MasterHolder(slotTable_, masterAudio_, bridgeShared_, masterTick_, tableChanged_, bridgeTicks_);
    holder_->setTableChangedHandler([this] { onTableChanged(); });
    holder_->setTickCounter(&clockTicks_);
    holder_->start();
  }
  bufferIndex_ = 0;
  samplePosition_ = 0;
  hwFillSum_ = 0;
  hwFillTicks_ = 0;
  sampleTimeNs_ = NowNs();
  clockStop_ = false;
  clockThread_ = CreateThread(nullptr, 0, clockProc, this, 0, nullptr);
  if (!clockThread_) {
    std::snprintf(errorText_, sizeof(errorText_), "Master Clock thread failed %lu", GetLastError());
    return ASE_HWMalfunction;
  }
  running_ = true;
  gStreaming = this;
  return ASE_OK;
}

WinHookMasterASIO* WinHookMasterASIO::streaming() { return gStreaming.load(); }

void WinHookMasterASIO::stats(WHAMasterStats* out) const {
  *out = WHAMasterStats{};
  out->ticks = clockTicks_.load();
  out->clockOverruns = clockOverruns_.load();
  out->workerOverruns = workerOverruns_.load();
  out->clockSource = clockSource_.load();
  out->hwMinFill = -1;
  if (!holder_) return;
  out->hwLastError = holder_->hwOpenError();
  if (KsAudio* hw = holder_->hwMaster()) {
    out->hwOpen = 1;
    out->hwWrites = hw->writes();
    out->hwUnderruns = hw->underruns();
    out->hwDrops = hw->drops();
    out->hwCapacity = hw->capacity();
    out->hwMinFill = hw->minFill();
    out->hwMaxFill = hw->maxFill();
    out->hwStreamLatency = hw->streamLatency();
    const uint64_t ticks = hwFillTicks_.load();
    out->hwFillAtTick = ticks ? static_cast<int32_t>(hwFillSum_.load() / ticks) : -1;
  }
  out->hwInLastError = holder_->hwCaptureError();
  if (KsCapture* in = holder_->hwCapture()) {
    const HwInputFifo& f = in->fifo();
    out->hwInOpen = 1;
    out->hwInReads = f.reads();
    out->hwInStarved = f.starved();
    out->hwInTrims = f.trims();
    out->hwInGlitches = in->glitches();
    out->hwInFill = f.fill();
    out->hwInTarget = f.expectedFill();
    out->hwInMeanFill = f.meanFill();
    out->hwInStreamLatency = in->streamLatency();
    out->hwInDriftPpmMilli = static_cast<int32_t>(f.sourcePpm() * 1000.0);
    out->hwInDriftEngaged = f.driftEngaged() ? 1 : 0;
    out->hwInGrowths = f.growths();
    out->hwInSkipped = f.skipped();
  }
}

ASIOError WinHookMasterASIO::stop() {
  if (gStreaming.load() == this) gStreaming = nullptr;
  if (clockThread_) {  // no bufferSwitch after stop() returns
    clockStop_ = true;
    WaitForSingleObject(clockThread_, INFINITE);
    CloseHandle(clockThread_);
    clockThread_ = nullptr;
  }
  if (holder_) { holder_->stop(); delete holder_; holder_ = nullptr; }
  running_ = false;
  return ASE_OK;
}

ASIOError WinHookMasterASIO::getChannels(long* numInputChannels, long* numOutputChannels) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  snapshotDawView();
  if (numInputChannels) *numInputChannels = static_cast<long>(slotTable_->masterInCount);
  if (numOutputChannels) *numOutputChannels = static_cast<long>(slotTable_->masterOutCount);
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getLatencies(long* inputLatency, long* outputLatency) {
  if (!initialized_) return ASE_NotPresent;
  // Frames from the sample position at bufferSwitch. Inputs and Worker-routed outputs: one block
  // (routed on the tick after the DAW wrote them). HW slots add the device queues. Not streaming
  // yet: open the devices briefly for their real buffers and stream latencies.
  const auto& g = slotTable_->general;
  bool hwIn = false, hwOut = false;
  for (uint32_t i = 0; i < slotTable_->masterInCount; ++i) hwIn = hwIn || slotTable_->masterIn[i].type == SLOT_HW;
  for (uint32_t i = 0; i < slotTable_->masterOutCount; ++i) hwOut = hwOut || slotTable_->masterOut[i].type == SLOT_HW;
  long output = bufferSize_, input = bufferSize_;
  if (KsAudio* hw = holder_ ? holder_->hwMaster() : nullptr) {
    output = HwOutputLatency(*hw);
  } else if (hwOut) {
    KsAudio probe;
    if (probe.open(static_cast<int32_t>(sampleRate_), static_cast<int32_t>(g.hwBuffer), bufferSize_, g.hwRenderId))
      output = HwOutputLatency(probe);
  }
  if (KsCapture* hw = holder_ ? holder_->hwCapture() : nullptr) {
    input = HwInputLatency(*hw, bufferSize_);
  } else if (hwIn) {
    KsCapture probe;
    if (probe.open(static_cast<int32_t>(sampleRate_), static_cast<int32_t>(g.hwBuffer), bufferSize_, g.hwCaptureId))
      input = HwInputLatency(probe, bufferSize_);
  }
  if (inputLatency) *inputLatency = input;
  if (outputLatency) *outputLatency = output;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  // The Master Clock buffer is chosen in the Control Panel (GENERAL); the DAW gets exactly that one.
  const long size = static_cast<long>(slotTable_->general.asioBuffer);
  if (minSize) *minSize = size;
  if (maxSize) *maxSize = size;
  if (preferredSize) *preferredSize = size;
  if (granularity) *granularity = 0;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::canSampleRate(ASIOSampleRate sampleRate) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  return static_cast<uint32_t>(sampleRate) == slotTable_->general.sampleRate ? ASE_OK : ASE_NoClock;
}
ASIOError WinHookMasterASIO::getSampleRate(ASIOSampleRate* sampleRate) {
  if (!sampleRate) return ASE_InvalidParameter;
  if (!slotTable_) return ASE_NotPresent;
  *sampleRate = static_cast<double>(slotTable_->general.sampleRate);
  return ASE_OK;
}
ASIOError WinHookMasterASIO::setSampleRate(ASIOSampleRate sampleRate) {
  if (sampleRate == 0.0) return ASE_OK;  // SDK: 0 = external clock request; we only have the Master Clock
  return canSampleRate(sampleRate);
}
ASIOError WinHookMasterASIO::getClockSources(ASIOClockSource* clocks, long* numSources) {
  if (!clocks || !numSources || *numSources < 1) return ASE_InvalidParameter;
  clocks[0] = ASIOClockSource{};
  clocks[0].index = 0;
  clocks[0].associatedChannel = -1;
  clocks[0].associatedGroup = -1;
  clocks[0].isCurrentSource = ASIOTrue;
  strcpy_s(clocks[0].name, sizeof(clocks[0].name), "Master Clock (Internal)");
  *numSources = 1;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::setClockSource(long reference) { return reference == 0 ? ASE_OK : ASE_InvalidMode; }
ASIOError WinHookMasterASIO::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
  if (!sPos || !tStamp) return ASE_InvalidParameter;
  if (!running_) return ASE_SPNotAdvancing;
  ToAsio64(samplePosition_.load(), &sPos->hi, &sPos->lo);
  ToAsio64(sampleTimeNs_.load(), &tStamp->hi, &tStamp->lo);
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getChannelInfo(ASIOChannelInfo* info) {
  if (!info || !slotTable_) return ASE_InvalidParameter;
  bool isInput = info->isInput != 0;
  long ch = info->channel;
  const WHASlot* slot = nullptr;
  if (isInput) {
    if (ch < 0 || ch >= static_cast<long>(slotTable_->masterInCount)) return ASE_InvalidParameter;
    slot = &slotTable_->masterIn[ch];
  } else {
    if (ch < 0 || ch >= static_cast<long>(slotTable_->masterOutCount)) return ASE_InvalidParameter;
    slot = &slotTable_->masterOut[ch];
  }
  info->isActive = ASIOFalse;
  for (const Binding& b : bindings_)
    if (b.isInput == isInput && b.channel == ch) info->isActive = ASIOTrue;  // SDK: active = has buffers
  info->channelGroup = 0;
  info->type = ASIOSTFloat32LSB;
  if (slot->type == SLOT_NONE) strcpy_s(info->name, sizeof(info->name), "- empty -");
  else TruncateCopy(info->name, sizeof(info->name), slot->name);
  return ASE_OK;
}

ASIOError WinHookMasterASIO::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                           ASIOCallbacks* callbacks) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  if (running_) return ASE_InvalidMode;
  if (!bufferInfos || !callbacks || !callbacks->bufferSwitch || numChannels <= 0) return ASE_InvalidParameter;
  if (bufferSize != static_cast<long>(slotTable_->general.asioBuffer)) return ASE_InvalidMode;  // Master Clock only
  if (buffersCreated_) disposeBuffers();
  for (long i = 0; i < numChannels; ++i) {
    const long limit = static_cast<long>(bufferInfos[i].isInput ? slotTable_->masterInCount : slotTable_->masterOutCount);
    if (bufferInfos[i].channelNum < 0 || bufferInfos[i].channelNum >= limit) return ASE_InvalidParameter;
  }
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

ASIOError WinHookMasterASIO::disposeBuffers() {
  if (running_) stop();
  bindings_.clear();
  dawBuffers_.clear();
  dawBuffers_.shrink_to_fit();
  buffersCreated_ = false;
  return ASE_OK;
}

DWORD WINAPI WinHookMasterASIO::clockProc(LPVOID self) {
  static_cast<WinHookMasterASIO*>(self)->runClock();
  return 0;
}

// Master Clock: one bufferSwitch per period. With a HW output open, the hardware is the clock:
// tick when its buffer has drained to the target fill, so the DAW produces exactly as fast as the
// device consumes. Without one, an internal timeline paced against absolute QPC time.
void WinHookMasterASIO::runClock() {
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));  // reads the Worker's IAudioClient
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // pre-1803: ~1 ms granularity

  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  const double qpcPerFrame = static_cast<double>(freq.QuadPart) / sampleRate_;
  const double periodQpc = static_cast<double>(bufferSize_) * qpcPerFrame;
  const DWORD periodMs = static_cast<DWORD>(std::ceil(1000.0 * static_cast<double>(bufferSize_) / sampleRate_));
  auto sleepQpc = [&](double ticks) {
    if (ticks <= 0 || !timer) return;
    LARGE_INTEGER rel;
    rel.QuadPart = -static_cast<LONGLONG>(ticks * 1e7 / static_cast<double>(freq.QuadPart));  // 100 ns units
    if (SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
  };
  QueryPerformanceCounter(&now);
  double due = static_cast<double>(now.QuadPart);
  while (!clockStop_.load()) {
    // Bounded wait for the Worker to route (and write to HW) the last tick; never block the DAW longer
    // than one period. Before the fill check, or the fill would not yet include that block.
    if (holder_ && !holder_->waitWorker(periodMs)) workerOverruns_.fetch_add(1);

    KsAudio* hw = holder_ ? holder_->hwMaster() : nullptr;
    const long target = hw ? hw->targetFill() : -1;
    long fill = target >= 0 ? hw->padding() : -1;
    while (fill > target && !clockStop_.load()) {
      sleepQpc(static_cast<double>(fill - target) * qpcPerFrame);
      fill = hw->padding();
    }
    if (clockStop_.load()) break;
    if (fill >= 0) {
      clockSource_ = CLOCK_HARDWARE;
      hwFillSum_.fetch_add(static_cast<uint64_t>(fill));
      hwFillTicks_.fetch_add(1);
      clockTick();
      QueryPerformanceCounter(&now);
      due = static_cast<double>(now.QuadPart) + periodQpc;  // internal timeline resumes from here if the HW goes
      continue;
    }

    QueryPerformanceCounter(&now);
    if (static_cast<double>(now.QuadPart) > due + kMaxCatchUpPeriods * periodQpc) {  // long stall: resync
      clockOverruns_.fetch_add(1);
      due = static_cast<double>(now.QuadPart);
    }
    sleepQpc(due - static_cast<double>(now.QuadPart));
    clockSource_ = CLOCK_INTERNAL;
    clockTick();
    due += periodQpc;
  }
  if (timer) CloseHandle(timer);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  if (com) CoUninitialize();
}

void WinHookMasterASIO::clockTick() {
  const long index = bufferIndex_;
  bufferIndex_ ^= 1;
  const size_t bytes = sizeof(float) * static_cast<size_t>(bufferSize_);
  // SHM IN slots (routed by the Worker last tick) -> DAW inputs
  for (const Binding& b : bindings_) {
    if (!b.isInput) continue;
    std::memcpy(b.buffers[index], masterAudio_ + (kMax + static_cast<size_t>(b.channel)) * kSlotFrames, bytes);
  }
  sampleTimeNs_ = NowNs();
  callbacks_.bufferSwitch(index, ASIOTrue);  // DAW processes now, on this thread
  // DAW outputs -> SHM OUT slots, then let the Worker route them
  for (const Binding& b : bindings_) {
    if (b.isInput) continue;
    std::memcpy(masterAudio_ + static_cast<size_t>(b.channel) * kSlotFrames, b.buffers[index], bytes);
  }
  samplePosition_ += static_cast<uint64_t>(bufferSize_);
  clockTicks_.fetch_add(1);
  // The HW input grew its backlog target after a starve: the DAW re-queries getLatencies.
  if (KsCapture* in = holder_ ? holder_->hwCapture() : nullptr) {
    if (in->takeLatencyChanged() && callbacks_.asioMessage &&
        callbacks_.asioMessage(kAsioSelectorSupported, kAsioLatenciesChanged, nullptr, nullptr) == 1)
      callbacks_.asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
  }
  if (masterTick_) SetEvent(masterTick_);
}

void WinHookMasterASIO::requestReset() {
  {
    std::lock_guard<std::mutex> lock(dawViewMutex_);
    if (resetPending_) return;  // one request until the DAW re-queries getChannels
    resetPending_ = true;
  }
  resetRequested_ = true;
  if (callbacks_.asioMessage && callbacks_.asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1)
    callbacks_.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
}

void WinHookMasterASIO::snapshotDawView() {
  std::lock_guard<std::mutex> lock(dawViewMutex_);
  dawView_ = *slotTable_;
  resetPending_ = false;
}

void WinHookMasterASIO::onTableChanged() {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(dawViewMutex_);
    changed = DawVisibleChanged(*slotTable_, dawView_);
  }
  if (changed) requestReset();  // e.g. a Bridge popup in another process saved new slots
}

ASIOError WinHookMasterASIO::controlPanel() {
#if WHA_HAVE_IMGUI
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  if (!panel_) panel_ = new ControlPanelWindow();
  ControlPanelHost host;
  host.table = slotTable_;
  host.tableChanged = tableChanged_;
  for (int i = 0; i < 4; ++i) host.bridges[i] = bridgeShared_[i];
  host.isMaster = true;
  host.onSaved = [this](bool reset) {
    if (reset) requestReset();
  };
  return panel_->Open(host) ? ASE_OK : ASE_NotPresent;
#else
  return ASE_NotPresent;  // offline build without ImGui: no popup
#endif
}

// SDK: unknown selectors -> ASE_InvalidParameter; supported ones would return ASE_SUCCESS (not ASE_OK).
// No time code, input monitoring, transport, gain or meters yet.
ASIOError WinHookMasterASIO::future(long selector, void* opt) {
  (void)selector;
  (void)opt;
  return ASE_InvalidParameter;
}

// Outputs are taken right after bufferSwitch returns (directProcess), so outputReady is not needed.
ASIOError WinHookMasterASIO::outputReady() { return ASE_NotPresent; }

}  // namespace wha
