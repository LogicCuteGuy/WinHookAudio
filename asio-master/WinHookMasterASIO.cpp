#include "WinHookMasterASIO.h"
#include "MasterHolder.h"

#include <cstring>
#include <cstdio>

namespace wha {

WinHookMasterASIO::WinHookMasterASIO() = default;
WinHookMasterASIO::~WinHookMasterASIO() {
  stop();
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
  if (riid == IID_IUnknown || riid == CLSID_WinHookMaster) {
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

ASIOError WinHookMasterASIO::init(void* sysHandle) {
  (void)sysHandle;
  if (initialized_) return ASE_OK;
  // Create SlotTable SHM 80KB
  slotTableMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(shm::kSlotTableSize), shm::kSlotTableName + 7);
  if (!slotTableMapping_) {
    std::snprintf(errorText_, sizeof(errorText_), "CreateFileMapping SlotTable failed %lu", GetLastError());
    return ASE_NoMemory;
  }
  slotTable_ = static_cast<WHASlotTable*>(MapViewOfFile(slotTableMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize));
  if (!slotTable_) return ASE_NoMemory;
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
  bufferSize_ = static_cast<int32_t>(slotTable_->general.asioBuffer);
  // Master audio 16MB
  masterAudioMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                           static_cast<DWORD>(shm::kMasterAudioSize), shm::kMasterAudioName + 7);
  if (masterAudioMapping_) masterAudio_ = static_cast<float*>(MapViewOfFile(masterAudioMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kMasterAudioSize));
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
  return ASE_OK;
}

void WinHookMasterASIO::getDriverName(char* name) { strcpy_s(name, 32, "WinHookAudio Master"); }
int32_t WinHookMasterASIO::getDriverVersion() { return 0x00010000; }
void WinHookMasterASIO::getErrorMessage(char* text) { strcpy_s(text, 128, errorText_); }
ASIOError WinHookMasterASIO::start() {
  if (!initialized_) return ASE_NotPresent;
  if (!holder_) {
    holder_ = new MasterHolder(slotTable_, masterAudio_, bridgeShared_, masterTick_, tableChanged_, bridgeTicks_);
    holder_->start();
  }
  running_ = true;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::stop() {
  if (holder_) { holder_->stop(); delete holder_; holder_ = nullptr; }
  running_ = false;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getChannels(int32_t* numInputChannels, int32_t* numOutputChannels) {
  if (!initialized_ || !slotTable_) return ASE_NotPresent;
  if (numInputChannels) *numInputChannels = static_cast<int32_t>(slotTable_->masterInCount);
  if (numOutputChannels) *numOutputChannels = static_cast<int32_t>(slotTable_->masterOutCount);
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getLatencies(int32_t* inputLatency, int32_t* outputLatency) {
  if (inputLatency) *inputLatency = bufferSize_;
  if (outputLatency) *outputLatency = bufferSize_;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) {
  if (minSize) *minSize = 64;
  if (maxSize) *maxSize = 1024;
  if (preferredSize) *preferredSize = bufferSize_;
  if (granularity) *granularity = 0;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::canSampleRate(double sampleRate) {
  int32_t sr = static_cast<int32_t>(sampleRate);
  if (sr == 44100 || sr == 48000 || sr == 96000) return ASE_OK;
  return ASE_NoClock;
}
ASIOError WinHookMasterASIO::getSampleRate(double* sampleRate) {
  if (!sampleRate) return ASE_InvalidParameter;
  if (!slotTable_) return ASE_NotPresent;
  *sampleRate = static_cast<double>(slotTable_->general.sampleRate);
  return ASE_OK;
}
ASIOError WinHookMasterASIO::setSampleRate(double sampleRate) { return canSampleRate(sampleRate); }
ASIOError WinHookMasterASIO::getClockSources(int64_t* clocks, int32_t* numSources) {
  (void)clocks;
  if (numSources) *numSources = 0;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::setClockSource(int32_t reference) { (void)reference; return ASE_OK; }
ASIOError WinHookMasterASIO::getSamplePosition(int64_t* sPos, int64_t* tPos) {
  if (sPos) *sPos = 0;
  if (tPos) *tPos = 0;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::getChannelInfo(ASIOChannelInfo* info) {
  if (!info || !slotTable_) return ASE_InvalidParameter;
  bool isInput = info->isInput != 0;
  int32_t ch = info->channel;
  const WHASlot* slot = nullptr;
  if (isInput) {
    if (ch < 0 || ch >= static_cast<int32_t>(slotTable_->masterInCount)) return ASE_InvalidParameter;
    slot = &slotTable_->masterIn[ch];
  } else {
    if (ch < 0 || ch >= static_cast<int32_t>(slotTable_->masterOutCount)) return ASE_InvalidParameter;
    slot = &slotTable_->masterOut[ch];
  }
  info->isActive = slot->enabled ? 1 : 0;
  info->channelGroup = 0;
  info->type = ASIOSTFloat32LSB;
  if (slot->type == SLOT_NONE) strcpy_s(info->name, sizeof(info->name), "- empty -");
  else {
    TruncateCopy(info->name, sizeof(info->name), slot->name);
  }
  return ASE_OK;
}
ASIOError WinHookMasterASIO::createBuffers(ASIOChannelInfo* infos, int32_t numChannels, int32_t bufferSize, ASIOCallbacks* callbacks) {
  (void)infos; (void)numChannels;
  if (callbacks) callbacks_ = *callbacks;
  bufferSize_ = bufferSize;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::disposeBuffers() { return ASE_OK; }
ASIOError WinHookMasterASIO::controlPanel() { return ASE_OK; }
ASIOError WinHookMasterASIO::future(int32_t selector, void* opt) {
  (void)selector; (void)opt;
  if (selector == kAsioResetRequest) resetRequested_ = true;
  return ASE_OK;
}
ASIOError WinHookMasterASIO::outputReady() {
  // 08: bufferSwitch does only memcpy + SetEvent(Master_Tick)
  // For offline test, simulate DAW In <- SHM In + SHM Out <- DAW Out + SetEvent
  if (masterTick_) SetEvent(masterTick_);
  return ASE_OK;
}

}  // namespace wha
