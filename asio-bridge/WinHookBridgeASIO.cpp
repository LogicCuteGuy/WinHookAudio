#include "WinHookBridgeASIO.h"
#include <cstring>
#include <cstdio>

namespace wha {

WinHookBridgeASIO::WinHookBridgeASIO(int bridgeIndex) : bridgeIndex_(bridgeIndex) {}
WinHookBridgeASIO::~WinHookBridgeASIO() {
  if (bridgeShared_ && clientId_ >= 0) {
    // Best effort: decrement not required for offline test
  }
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
ULONG STDMETHODCALLTYPE WinHookBridgeASIO::Release() { ULONG c = --refCount_; if (c==0) delete this; return c; }

int WinHookBridgeASIO::countBridgeChannels() const {
  if (!slotTable_) return 0;
  WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
  int n = 0;
  for (uint32_t i = 0; i < slotTable_->masterInCount; ++i) if (slotTable_->masterIn[i].type == want) ++n;
  return n;
}
const WHASlot* WinHookBridgeASIO::bridgeSlot(bool isInput, int32_t n) const {
  if (!slotTable_) return nullptr;
  WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bridgeIndex_);
  int idx = 0;
  if (isInput) {
    for (uint32_t i = 0; i < slotTable_->masterInCount; ++i) if (slotTable_->masterIn[i].type == want) { if (idx++ == n) return &slotTable_->masterIn[i]; }
  } else {
    for (uint32_t i = 0; i < slotTable_->masterOutCount; ++i) if (slotTable_->masterOut[i].type == want) { if (idx++ == n) return &slotTable_->masterOut[i]; }
  }
  return nullptr;
}

ASIOError WinHookBridgeASIO::init(void* sysHandle) {
  (void)sysHandle;
  if (initialized_) return ASE_OK;
  slotTableMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kSlotTableName + 7);
  if (!slotTableMapping_) {
    // For offline test without Master, create a minimal table
    slotTableMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(shm::kSlotTableSize), shm::kSlotTableName + 7);
    if (!slotTableMapping_) return ASE_NotPresent;
  }
  slotTable_ = static_cast<WHASlotTable*>(MapViewOfFile(slotTableMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize));
  if (!slotTable_) return ASE_NoMemory;
  // Bridge shared
  bridgeMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kBridgeSharedNames[bridgeIndex_] + 7);
  if (!bridgeMapping_) {
    bridgeMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(shm::kBridgeSharedSize), shm::kBridgeSharedNames[bridgeIndex_] + 7);
    if (!bridgeMapping_) return ASE_NotPresent;
  }
  bridgeShared_ = static_cast<WHABridgeShared*>(MapViewOfFile(bridgeMapping_, FILE_MAP_ALL_ACCESS, 0, 0, shm::kBridgeSharedSize));
  if (!bridgeShared_) return ASE_NoMemory;
  // Allocate clientId
  if (!TryAddBridgeClient(*bridgeShared_, &clientId_)) {
    std::snprintf(errorText_, sizeof(errorText_), "Bridge%d full (4/4) — use Bridge2/3/4", bridgeIndex_+1);
    return ASE_NotPresent;
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
    slotTable_->version = 1;
  }
  bufferSize_ = static_cast<int32_t>(slotTable_->general.asioBuffer);
  initialized_ = true;
  return ASE_OK;
}

void WinHookBridgeASIO::getDriverName(char* name) { std::snprintf(name, 32, "WinHookAudio Bridge %d", bridgeIndex_+1); }
int32_t WinHookBridgeASIO::getDriverVersion() { return 0x00010000; }
void WinHookBridgeASIO::getErrorMessage(char* text) { strcpy_s(text, 128, errorText_); }
ASIOError WinHookBridgeASIO::start() { return initialized_ ? ASE_OK : ASE_NotPresent; }
ASIOError WinHookBridgeASIO::stop() { return ASE_OK; }
ASIOError WinHookBridgeASIO::getChannels(int32_t* numInputChannels, int32_t* numOutputChannels) {
  if (!initialized_) return ASE_NotPresent;
  int n = countBridgeChannels();
  // Bridge exposes same count for in/out (subset)
  if (numInputChannels) *numInputChannels = n;
  if (numOutputChannels) *numOutputChannels = n;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::getLatencies(int32_t* inputLatency, int32_t* outputLatency) {
  if (inputLatency) *inputLatency = bufferSize_;
  if (outputLatency) *outputLatency = bufferSize_;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) {
  if (minSize) *minSize = 64;
  if (maxSize) *maxSize = 1024;
  if (preferredSize) *preferredSize = bufferSize_;
  if (granularity) *granularity = 0;
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::canSampleRate(double sr) { int32_t v=static_cast<int32_t>(sr); return (v==44100||v==48000||v==96000)?ASE_OK:ASE_NoClock; }
ASIOError WinHookBridgeASIO::getSampleRate(double* sr) { if(!sr||!slotTable_) return ASE_InvalidParameter; *sr=static_cast<double>(slotTable_->general.sampleRate); return ASE_OK; }
ASIOError WinHookBridgeASIO::setSampleRate(double sr) { return canSampleRate(sr); }
ASIOError WinHookBridgeASIO::getClockSources(int64_t* c,int32_t* n){(void)c; if(n) *n=0; return ASE_OK;}
ASIOError WinHookBridgeASIO::setClockSource(int32_t r){(void)r; return ASE_OK;}
ASIOError WinHookBridgeASIO::getSamplePosition(int64_t* s,int64_t* t){if(s) *s=0; if(t) *t=0; return ASE_OK;}
ASIOError WinHookBridgeASIO::getChannelInfo(ASIOChannelInfo* info) {
  if (!info || !slotTable_) return ASE_InvalidParameter;
  const WHASlot* slot = bridgeSlot(info->isInput!=0, info->channel);
  if (!slot) return ASE_InvalidParameter;
  info->isActive = slot->enabled ? 1 : 0;
  info->channelGroup = 0;
  info->type = ASIOSTFloat32LSB;
  if (slot->type == SLOT_NONE) strcpy_s(info->name, sizeof(info->name), "- empty -");
  else TruncateCopy(info->name, sizeof(info->name), slot->name);
  return ASE_OK;
}
ASIOError WinHookBridgeASIO::createBuffers(ASIOChannelInfo* infos,int32_t numChannels,int32_t bufferSize,ASIOCallbacks* cb){(void)infos;(void)numChannels; (void)cb; bufferSize_=bufferSize; return ASE_OK;}
ASIOError WinHookBridgeASIO::disposeBuffers(){return ASE_OK;}
ASIOError WinHookBridgeASIO::controlPanel(){return ASE_OK;}
ASIOError WinHookBridgeASIO::future(int32_t s,void* o){(void)s;(void)o; return ASE_OK;}
ASIOError WinHookBridgeASIO::outputReady(){
  // Bridge: Slave DAW OUT -> clientIn[clientId][active] and wait on Bridge_Tick
  // For 08 offline, just signal ready
  if (bridgeShared_ && clientId_>=0) bridgeShared_->ready[clientId_] = 1;
  return ASE_OK;
}

}  // namespace wha
