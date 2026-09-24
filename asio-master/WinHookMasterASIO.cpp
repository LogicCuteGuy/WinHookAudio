#include "WinHookMasterASIO.h"
#include "MasterHolder.h"
#include "KsAudio.h"
#include "KsCapture.h"
#include "HwOutputFifo.h"
#include "WHASlotsFile.h"
#include "virtual/WHACableProtocol.h"
#if WHA_HAVE_IMGUI
#include "WHAControlPanelWindow.h"
#endif

#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "avrt.lib")

namespace wha {

namespace {

constexpr size_t kSlotFrames = 4096;  // Master audio SHM stride per slot
static_assert(kStatsEndpointIdLen == kEndpointIdLen, "WHAMasterStats endpoint IDs hold a Slot Table ID");
static_assert(kStatsHwMore == kHwDevices - 1, "WHAMasterStats has one entry per more HW device");
static_assert(static_cast<int>(KsSampleFormat::Float32) == HW_FORMAT_FLOAT32 &&
                  static_cast<int>(KsSampleFormat::Pcm24In32) == HW_FORMAT_PCM24IN32 &&
                  static_cast<int>(KsSampleFormat::Pcm16) == HW_FORMAT_PCM16,
              "WHAHwFormat mirrors KsSampleFormat");
// Internal timeline: after a stall shorter than this, tick back-to-back to catch up (the average
// rate stays exact); after a longer one, resync instead of bursting.
constexpr double kMaxCatchUpPeriods = 8;

// A block handed over at a HW Master Clock tick is queued behind the device's fill at the tick, then
// crosses the device. The fill: the pacer's expected fill while streaming, else the first threshold.
long HwOutputLatency(const KsAudio& hw, int32_t pacedFill = -1) {
  return (pacedFill >= 0 ? pacedFill : hw.targetFill()) + hw.streamLatency();
}
// A captured frame's age when read (the backlog target, from capture timestamps, plus the
// resampler), then one tick in its IN slot.
long HwInputLatency(const KsCapture& hw, long block) { return hw.latency() + block; }
// A more output device (own clock): the block waits behind the FIFO's backlog and the resampler,
// then crosses the device.
long HwMoreOutputLatency(const KsAudio& hw, const HwOutputFifo& fifo) {
  return fifo.target() + HwOutputFifo::resamplerDelay() + hw.streamLatency();
}

// The lowest HW device index a HW slot of one direction uses (-1: none).
int FirstUsedDevice(const WHASlotTable& t, bool isInput) {
  const WHASlot* slots = isInput ? t.masterIn : t.masterOut;
  const uint32_t count = isInput ? t.masterInCount : t.masterOutCount;
  int first = -1;
  for (uint32_t i = 0; i < count && i < kMax; ++i)
    if (slots[i].type == SLOT_HW && HwDeviceListed(t, isInput, HwDeviceOf(slots[i])) &&
        (first < 0 || HwDeviceOf(slots[i]) < first))
      first = HwDeviceOf(slots[i]);
  return first;
}

std::atomic<WinHookMasterASIO*> gStreaming{nullptr};

// Friendly name of a HW endpoint (UTF-8), "" if unknown. id "" = the Windows default of `flow`.
std::string EndpointFriendlyName(EDataFlow flow, const char* id) {
  std::string name;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* e = nullptr;
  IMMDevice* d = nullptr;
  IPropertyStore* props = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  if (SUCCEEDED(hr)) {
    if (id && *id) {
      wchar_t wid[128] = {};
      MultiByteToWideChar(CP_UTF8, 0, id, -1, wid, 128);
      hr = e->GetDevice(wid, &d);
    } else {
      hr = e->GetDefaultAudioEndpoint(flow, eConsole, &d);
    }
  }
  PROPVARIANT v;
  PropVariantInit(&v);
  if (SUCCEEDED(hr) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) &&
      SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
    char utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, utf8, sizeof(utf8), nullptr, nullptr);
    name = utf8;
  }
  PropVariantClear(&v);
  if (props) props->Release();
  if (d) d->Release();
  if (e) e->Release();
  if (SUCCEEDED(init)) CoUninitialize();  // RPC_E_CHANGED_MODE: the DAW's apartment, keep it
  return name;
}

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
  // First process to map the table: the saved Slot Table (Control Panel Save), else defaults.
  if (slotTable_->version == 0 && LoadConfigFiles(*slotTable_, nullptr)) {
    // loaded: routing, names and HW devices as last saved
  } else if (slotTable_->version == 0) {
    FillDefaultSlotTable(*slotTable_);
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
    auto* holder = new MasterHolder(slotTable_, masterAudio_, bridgeShared_, masterTick_, tableChanged_, bridgeTicks_);
    holder->setTableChangedHandler([this] { onTableChanged(); });
    holder->setTickCounter(&clockTicks_);
    {
      std::lock_guard<std::mutex> lock(holderMutex_);
      holder_ = holder;
    }
    holder_->start();
  }
  bufferIndex_ = 0;
  samplePosition_ = 0;
  hwFillSum_ = 0;
  hwFillTicks_ = 0;
  pacedHw_ = nullptr;
  hwOutReportFill_ = -1;
  hwOutLatencyChanged_ = false;
  hwChunk_ = 0;
  hwHurries_ = 0;
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

bool WinHookMasterASIO::stats(WHAMasterStats* out) const {
  *out = WHAMasterStats{};
  out->ticks = clockTicks_.load();
  out->clockOverruns = clockOverruns_.load();
  out->workerOverruns = workerOverruns_.load();
  out->clockSource = clockSource_.load();
  out->sampleRate = static_cast<int32_t>(sampleRate_);
  out->asioBuffer = static_cast<int32_t>(bufferSize_);
  out->hwMinFill = -1;
  out->hwFormat = out->hwInFormat = HW_FORMAT_NONE;
  std::lock_guard<std::mutex> lock(holderMutex_);
  if (!holder_) return false;
  WHAGeneral request{};
  WHAHwMore requestMore{};
  if (holder_->hwRequest(request, &requestMore)) {
    out->hwRequestValid = 1;
    out->hwRequestedPeriod = static_cast<int32_t>(request.hwBuffer);
    TruncateCopy(out->hwRequestedRenderId, kStatsEndpointIdLen, request.hwRenderId);
    TruncateCopy(out->hwRequestedCaptureId, kStatsEndpointIdLen, request.hwCaptureId);
  }
  out->hwLastError = holder_->hwOpenError();
  if (KsAudio* hw = holder_->hwMaster()) {
    out->hwOpen = 1;
    out->hwPeriod = hw->periodFrames();
    out->hwFormat = static_cast<int32_t>(hw->format());
    out->hwLatency = static_cast<int32_t>(HwOutputLatency(*hw, hwOutReportFill_.load()));
    out->hwChunk = hwChunk_.load();
    out->hwHurries = hwHurries_.load();
    TruncateCopy(out->hwRenderId, kStatsEndpointIdLen, hw->endpointId().c_str());
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
    out->hwInPeriod = in->periodFrames();
    out->hwInFormat = static_cast<int32_t>(in->format());
    out->hwInLatency = static_cast<int32_t>(HwInputLatency(*in, bufferSize_));
    TruncateCopy(out->hwCaptureId, kStatsEndpointIdLen, in->endpointId().c_str());
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
  for (int d = 1; d < kHwDevices; ++d) {
    WHAHwDeviceStats& o = out->hwMoreOut[d - 1];
    TruncateCopy(o.requestedId, kStatsEndpointIdLen, requestMore.renderId[d - 1]);
    o.used = holder_->hwOutputUsed(d) ? 1 : 0;
    o.lastError = holder_->hwOpenError(d);
    o.format = HW_FORMAT_NONE;
    const int route = holder_->hwOutputRoute(d);
    o.sameAs = route >= 0 && route != d ? route : -1;
    const HwOutputFifo* fifo = holder_->hwOutputFifo(d);
    if (KsAudio* hw = holder_->hwOutput(d); hw && fifo) {
      o.open = 1;
      TruncateCopy(o.id, kStatsEndpointIdLen, hw->endpointId().c_str());
      o.period = hw->periodFrames();
      o.format = static_cast<int32_t>(hw->format());
      o.latency = static_cast<int32_t>(HwMoreOutputLatency(*hw, *fifo));
      o.fill = fifo->fill();
      o.target = fifo->target();
      o.driftPpmMilli = static_cast<int32_t>(fifo->devicePpm() * 1000.0);
      o.driftEngaged = fifo->driftEngaged() ? 1 : 0;
      o.chunk = fifo->chunk();
      o.blocks = fifo->blocks();
      o.underruns = fifo->underruns();
      o.gaps = fifo->gaps();
      o.trims = fifo->trims();
    }
    WHAHwDeviceStats& i = out->hwMoreIn[d - 1];
    TruncateCopy(i.requestedId, kStatsEndpointIdLen, requestMore.captureId[d - 1]);
    i.used = holder_->hwInputUsed(d) ? 1 : 0;
    i.lastError = holder_->hwCaptureError(d);
    i.format = HW_FORMAT_NONE;
    const int inRoute = holder_->hwInputRoute(d);
    i.sameAs = inRoute >= 0 && inRoute != d ? inRoute : -1;
    if (KsCapture* in = holder_->hwCapture(d)) {
      const HwInputFifo& f = in->fifo();
      i.open = 1;
      TruncateCopy(i.id, kStatsEndpointIdLen, in->endpointId().c_str());
      i.period = in->periodFrames();
      i.format = static_cast<int32_t>(in->format());
      i.latency = static_cast<int32_t>(HwInputLatency(*in, bufferSize_));
      i.fill = f.fill();
      i.target = f.expectedFill();
      i.driftPpmMilli = static_cast<int32_t>(f.sourcePpm() * 1000.0);
      i.driftEngaged = f.driftEngaged() ? 1 : 0;
      i.blocks = f.reads();
      i.underruns = f.starved();
      i.gaps = in->glitches();
      i.trims = f.trims();
      i.growths = f.growths();
      i.skipped = f.skipped();
    }
  }
  out->cableDriverCables = holder_->cableDriverCables();
  out->cableDriverError = holder_->cableDriverError();
  for (int c = 0; c < kStatsCables; ++c) {
    WHACableStats& cs = out->cables[c];
    WHACableExchange reply{};
    uint64_t exchanges = 0, errors = 0;
    if (!holder_->cableStatus(c, reply, exchanges, errors)) continue;
    cs.used = 1;
    cs.playRate = static_cast<int32_t>(reply.playRate);
    cs.recordRate = static_cast<int32_t>(reply.recordRate);
    cs.playFill = static_cast<int32_t>(reply.playFill);
    cs.recordFill = static_cast<int32_t>(reply.recordFill);
    cs.exchanges = exchanges;
    cs.errors = errors;
    cs.playUnderruns = reply.playUnderruns;
    cs.playDrops = reply.playDrops;
    cs.recordUnderruns = reply.recordUnderruns;
    cs.recordDrops = reply.recordDrops;
  }
  return true;
}

ASIOError WinHookMasterASIO::stop() {
  if (gStreaming.load() == this) gStreaming = nullptr;
  if (clockThread_) {  // no bufferSwitch after stop() returns
    clockStop_ = true;
    WaitForSingleObject(clockThread_, INFINITE);
    CloseHandle(clockThread_);
    clockThread_ = nullptr;
  }
  if (holder_) {
    std::lock_guard<std::mutex> lock(holderMutex_);  // a stats() caller finishes first
    holder_->stop();
    delete holder_;
    holder_ = nullptr;
  }
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
  // With several HW devices the pair describes the Master Clock's output device and the lowest-numbered
  // input device in use (the others' own latencies are in the Control Panel).
  const auto& g = slotTable_->general;
  const int firstIn = FirstUsedDevice(*slotTable_, true);
  const bool hwIn = firstIn >= 0, hwOut = hwIn || FirstUsedDevice(*slotTable_, false) >= 0;
  long output = bufferSize_, input = bufferSize_;
  if (KsAudio* hw = holder_ ? holder_->hwMaster() : nullptr) {
    output = HwOutputLatency(*hw, hwOutReportFill_.load());
  } else if (hwOut) {
    KsAudio probe;
    if (probe.open(static_cast<int32_t>(sampleRate_), static_cast<int32_t>(g.hwBuffer), bufferSize_, g.hwRenderId))
      output = HwOutputLatency(probe);
  }
  if (KsCapture* hw = reportedCapture()) {
    input = HwInputLatency(*hw, bufferSize_);
  } else if (hwIn) {
    KsCapture probe;
    if (probe.open(static_cast<int32_t>(sampleRate_), static_cast<int32_t>(g.hwBuffer), bufferSize_,
                   HwDeviceId(*slotTable_, true, firstIn)))
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
  const long count = static_cast<long>(isInput ? slotTable_->masterInCount : slotTable_->masterOutCount);
  if (ch < 0 || ch >= count) return ASE_InvalidParameter;
  info->isActive = ASIOFalse;
  for (const Binding& b : bindings_)
    if (b.isInput == isInput && b.channel == ch) info->isActive = ASIOTrue;  // SDK: active = has buffers
  info->channelGroup = 0;
  info->type = ASIOSTFloat32LSB;
  char name[kNameLen];
  const char* devices[kHwDevices];
  for (int d = 0; d < kHwDevices; ++d) devices[d] = (isInput ? hwInNames_ : hwOutNames_)[d].c_str();
  if (isInput)
    DawChannelName(slotTable_->masterIn, slotTable_->masterInCount, static_cast<uint32_t>(ch), true, devices, name);
  else
    DawChannelName(slotTable_->masterOut, slotTable_->masterOutCount, static_cast<uint32_t>(ch), false, devices, name);
  TruncateCopy(info->name, sizeof(info->name), name);
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
  // Development trace (WINHOOKAUDIO_CLOCK_TRACE=<csv path>): every look at the HW output, written at
  // stop. Preallocated: no allocation on this thread while streaming.
  struct TraceRow { double sec; int64_t written; int32_t fill; double wait; int32_t tick; };
  std::vector<TraceRow> trace;
  char tracePath[MAX_PATH] = {};
  if (GetEnvironmentVariableA("WINHOOKAUDIO_CLOCK_TRACE", tracePath, MAX_PATH) > 0) trace.reserve(2000000);
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
    if (holder_ && !holder_->waitWorker(clockTicks_.load(), periodMs)) workerOverruns_.fetch_add(1);

    // HW Master Clock: tick when the pacer says, evenly spaced at the device's rate (HwClockPacer);
    // look at the device at least every half block to see how it reports its position.
    KsAudio* hw = holder_ ? holder_->hwMaster() : nullptr;
    long fill = -1;
    if (hw && hw != pacedHw_) {
      pacer_.reset(sampleRate_, static_cast<int>(bufferSize_), hw->capacity());
      pacedHw_ = hw;
    }
    while (hw && !clockStop_.load()) {
      int64_t written = 0;
      fill = hw->padding(&written);  // one snapshot: a Worker write between two reads looks like play
      if (fill < 0) break;
      QueryPerformanceCounter(&now);
      const double nowSec = static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
      pacer_.observe(nowSec, written, static_cast<int32_t>(fill));
      const double wait = pacer_.untilTick(nowSec);
      if (trace.capacity() && trace.size() < trace.capacity())
        trace.push_back({nowSec, written, static_cast<int32_t>(fill), wait, wait <= 0 ? 1 : 0});
      if (wait <= 0) break;
      const double pollQpc = 0.5 * periodQpc;
      const double waitQpc = wait * static_cast<double>(freq.QuadPart);
      sleepQpc(waitQpc < pollQpc ? waitQpc : pollQpc);
    }
    if (clockStop_.load()) break;
    if (fill >= 0) {
      clockSource_ = CLOCK_HARDWARE;
      hwFillSum_.fetch_add(static_cast<uint64_t>(fill));
      hwFillTicks_.fetch_add(1);
      hwChunk_ = pacer_.chunk();
      hwHurries_ = pacer_.hurries();
      // Output latency follows the pacer's fill (it drops by about half a chunk on a chunky device);
      // tell the DAW when it moved by a block or more from what it was told.
      const int32_t expected = pacer_.expectedFill();
      const int32_t reported = hwOutReportFill_.load() >= 0 ? hwOutReportFill_.load() : hw->targetFill();
      if (expected - reported >= bufferSize_ || reported - expected >= bufferSize_) {
        hwOutReportFill_ = expected;
        hwOutLatencyChanged_ = true;
      } else if (hwOutReportFill_.load() < 0) {
        hwOutReportFill_ = reported;
      }
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
  if (!trace.empty()) {
    FILE* f = nullptr;
    if (fopen_s(&f, tracePath, "w") == 0 && f) {
      std::fprintf(f, "sec,written,fill,wait,tick,chunk\n");
      for (const TraceRow& r : trace)
        std::fprintf(f, "%.6f,%lld,%d,%.6f,%d,%d\n", r.sec, static_cast<long long>(r.written), r.fill, r.wait, r.tick, pacer_.chunk());
      std::fclose(f);
    }
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
  // The HW output's pacing moved its fill: the DAW re-queries getLatencies too.
  KsCapture* in = reportedCapture();
  const bool inChanged = in && in->takeLatencyChanged();
  const bool outChanged = hwOutLatencyChanged_.exchange(false);
  if ((inChanged || outChanged) && callbacks_.asioMessage &&
      callbacks_.asioMessage(kAsioSelectorSupported, kAsioLatenciesChanged, nullptr, nullptr) == 1)
    callbacks_.asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
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
  {
    std::lock_guard<std::mutex> lock(dawViewMutex_);
    dawView_ = *slotTable_;
    resetPending_ = false;
  }
  // Automatic HW channel names carry the device's name ("Microphone L"); the DAW asks for them next.
  for (int d = 0; d < kHwDevices; ++d) {
    hwInNames_[d] = HwDeviceListed(dawView_, true, d) ? EndpointFriendlyName(eCapture, HwDeviceId(dawView_, true, d)) : "";
    hwOutNames_[d] = HwDeviceListed(dawView_, false, d) ? EndpointFriendlyName(eRender, HwDeviceId(dawView_, false, d)) : "";
  }
}

KsCapture* WinHookMasterASIO::reportedCapture() const {
  for (int d = 0; holder_ && d < kHwDevices; ++d)
    if (KsCapture* in = holder_->hwCapture(d)) return in;
  return nullptr;
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
  host.readStats = [this](WHAMasterStats& s) { return stats(&s); };
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
