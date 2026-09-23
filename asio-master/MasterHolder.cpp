#include "MasterHolder.h"
#include "KsAudio.h"
#include "KsCapture.h"
#include "network/WHANetworkEngine.h"
#include "virtual/WHAIoctl.h"
#include "virtual/WHARingBuffer.h"
#include <cmath>
#include <cstring>
#include <memory>

namespace wha {

MasterHolder::MasterHolder(WHASlotTable* table, float* masterAudio, WHABridgeShared* bridges[4],
                           HANDLE masterTick, HANDLE tableChanged, HANDLE bridgeTicks[4][4])
    : table_(table), masterAudio_(masterAudio), masterTick_(masterTick), tableChanged_(tableChanged) {
  for (int i = 0; i < 4; ++i) bridges_[i] = bridges[i];
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) bridgeTicks_[i][j] = bridgeTicks[i][j];
  workerDone_ = CreateEventA(nullptr, FALSE, TRUE, nullptr);  // signaled: the first tick has nothing to wait for
}
MasterHolder::~MasterHolder() { stop(); if (workerDone_) CloseHandle(workerDone_); delete network_; delete ksAudio_; delete ksCapture_; for (int i = 0; i < 8; ++i) delete virtualRings_[i]; }

bool MasterHolder::start() {
  if (running_) return true;
  // Preallocate virtual rings at start, not per-tick
  for (int i = 0; i < 8; ++i) if (!virtualRings_[i]) virtualRings_[i] = new WHARingBuffer();
  if (!network_) network_ = new WHANetworkEngine(table_);
  network_->start();  // socket opens only once a Network Stream is mapped
  stopRequested_ = false;
  running_ = true;
  DWORD id = 0;
  thread_ = CreateThread(nullptr, 0, threadProc, this, 0, &id);
  if (!thread_) { running_ = false; return false; }
  return true;
}
void MasterHolder::stop() {
  if (!running_) return;
  stopRequested_ = true;
  if (masterTick_) SetEvent(masterTick_);
  if (tableChanged_) SetEvent(tableChanged_);
  // Join without a timeout: the Worker's teardown (device Stop/Release, CoUninitialize) runs on this
  // object and in this DLL. Abandoning it after a timeout let it run on a deleted MasterHolder, whose
  // garbage state made an extra CoUninitialize tear down its apartment, and COM then unloaded the
  // DLL under the running thread (crash seen with a slow-closing capture device).
  if (thread_) {
    if (WaitForSingleObject(thread_, 1000) == WAIT_TIMEOUT) {
      OutputDebugStringA("WinHookAudio MasterHolder: Worker teardown > 1 s, still waiting\n");
      WaitForSingleObject(thread_, INFINITE);
    }
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (network_) network_->stop();
  running_ = false;
  if (workerDone_) SetEvent(workerDone_);
}
DWORD WINAPI MasterHolder::threadProc(LPVOID param) { static_cast<MasterHolder*>(param)->run(); return 0; }
void MasterHolder::run() {
  // MMCSS Pro Audio on worker thread
  avrtModule_ = LoadLibraryA("avrt.dll");
  if (avrtModule_) {
    using AvSetMmThreadCharacteristicsA = HANDLE(WINAPI*)(LPCSTR, LPDWORD);
    auto pfn = reinterpret_cast<AvSetMmThreadCharacteristicsA>(GetProcAddress(avrtModule_, "AvSetMmThreadCharacteristicsA"));
    if (pfn) { DWORD idx = 0; mmcssHandle_ = pfn("Pro Audio", &idx); }
  }
  // Open KS exclusive for HW slots (deferred if no HW)
  if (!ksAudio_) ksAudio_ = new KsAudio();
  if (!ksCapture_) ksCapture_ = new KsCapture();
  bool hwIn = false, hwOut = false;
  for (uint32_t i = 0; i < table_->masterInCount; ++i) hwIn = hwIn || table_->masterIn[i].type == SLOT_HW;
  for (uint32_t i = 0; i < table_->masterOutCount; ++i) hwOut = hwOut || table_->masterOut[i].type == SLOT_HW;
  const auto& g = table_->general;
  if (hwIn || hwOut) {
    // Opened and started, the HW output becomes the Master Clock (see WinHookMasterASIO::runClock).
    if (ksAudio_->open(g.sampleRate, g.hwBuffer, g.asioBuffer, g.hwRenderId) && ksAudio_->start())
      hwMaster_ = ksAudio_;
    else
      hwOpenError_ = static_cast<int32_t>(ksAudio_->lastError());
  }
  if (hwIn) {
    if (ksCapture_->open(g.sampleRate, g.hwBuffer, g.asioBuffer, g.hwCaptureId) && ksCapture_->start())
      hwCapture_ = ksCapture_;
    else
      hwCaptureError_ = static_cast<int32_t>(ksCapture_->lastError());
  }
  HANDLE handles[2] = {masterTick_, tableChanged_};
  int nHandles = (masterTick_ && tableChanged_) ? 2 : (masterTick_ ? 1 : 0);
  while (!stopRequested_) {
    DWORD wait = (nHandles > 0) ? WaitForMultipleObjects(nHandles, handles, FALSE, INFINITE) : WaitForSingleObject(masterTick_, INFINITE);
    if (stopRequested_) break;
    if (wait == WAIT_OBJECT_0 + 1) {  // TableChanged
      if (network_) network_->requestReconfigure();
      if (onTableChanged_) onTableChanged_();
    }
    // Route on Master_Tick only. A TableChanged-only wake used to run doTick too: an extra HW input
    // read / HW output write / Network block that no tick asked for. The next tick uses the new table.
    if (wait == WAIT_OBJECT_0) {
      if (stopRequested_) break;
      if (clockTicks_) {
        const uint64_t now = clockTicks_->load();
        if (haveTicks_ && now > ticksSeen_ + 1 && ksCapture_ && ksCapture_->opened())
          ksCapture_->skip(static_cast<size_t>(now - ticksSeen_ - 1) * table_->general.asioBuffer);
        ticksSeen_ = now;
        haveTicks_ = true;
      }
      doTick();
      if (workerDone_) SetEvent(workerDone_);  // Master_Tick routed
    }
  }
  hwMaster_ = nullptr;  // the Master Clock is already stopped; never hand out a closing device
  hwCapture_ = nullptr;
  if (ksAudio_) { ksAudio_->stop(); ksAudio_->close(); }
  if (ksCapture_) { ksCapture_->stop(); ksCapture_->close(); }
  if (mmcssHandle_ && avrtModule_) {
    using AvRevertMmThreadCharacteristics = BOOL(WINAPI*)(HANDLE);
    auto pfn = reinterpret_cast<AvRevertMmThreadCharacteristics>(GetProcAddress(avrtModule_, "AvRevertMmThreadCharacteristics"));
    if (pfn) pfn(mmcssHandle_);
    FreeLibrary(avrtModule_);
    avrtModule_ = nullptr;
    mmcssHandle_ = nullptr;
  }
}
void MasterHolder::tickOnce() {
  for (int i = 0; i < 8; ++i) if (!virtualRings_[i]) virtualRings_[i] = new WHARingBuffer();
  doTick();
}

void MasterHolder::doTick() {
  if (!table_ || !masterAudio_) return;
  // Per-thing FIFOs: for now, just handle Loopback OUT->IN next tick and Bridge sum
  // Loopback: if masterOut[i].loopback && type==VIRTUAL, copy OUT->IN next tick
  // Master audio is float[2][512][4096] ping-pong; for 09 offline, simulate with masterAudio_ as flat
  // For offline test, masterAudio_ is 16MB = 2*512*4096*4; use active buffer 0
  // Loopback: copy OUT slot's audio to paired IN slot's audio (same Virtual index)
  // Loopback: only OUT loopback matters, find IN by srcChannel (IN loopback not required)
  for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
    const auto& outSlot = table_->masterOut[oi];
    if (outSlot.loopback && outSlot.type == SLOT_VIRTUAL) {
      for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
        const auto& inSlot = table_->masterIn[ii];
        if (inSlot.type == SLOT_VIRTUAL && inSlot.srcChannel == outSlot.srcChannel) {
          int frames = static_cast<int>(table_->general.asioBuffer);
          if (frames > 4096) frames = 4096;
          // Flat layout for 09 offline: OUT at oi*4096, IN at 512*4096+ii*4096
          float* outBuf = masterAudio_ + oi * 4096;
          float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;
          std::memcpy(inBuf, outBuf, frames * sizeof(float));
          break;
        }
      }
    }
  }
  // Shared Bridge sum: stack-allocated sum, frames from asioBuffer, toggle mixedActive
  for (int bi = 0; bi < 4; ++bi) {
    auto* b = bridges_[bi];
    if (!b) continue;
    int frames = static_cast<int>(table_->general.asioBuffer);
    if (frames > static_cast<int>(kBridgeFrames)) frames = static_cast<int>(kBridgeFrames);
    if (frames <= 0) frames = 128;
    // Stack sum for max 64*128 frames (avoid heap per tick)
    float sum[64 * 128] = {};
    // For larger frames, use heap fallback (not on audio path in 09 offline)
    float* sumPtr = sum;
    std::unique_ptr<float[]> heapSum;
    if (frames > 128) {
      heapSum = std::make_unique<float[]>(64 * frames);
      std::memset(heapSum.get(), 0, 64 * frames * sizeof(float));
      sumPtr = heapSum.get();
    }
    int nReady = 0;
    for (int ci = 0; ci < 4; ++ci) {
      if (b->ready[ci]) {
        int active = b->activeBuf[ci] ? 1 : 0;
        for (int ch = 0; ch < 64; ++ch) {
          for (int f = 0; f < frames; ++f) {
            sumPtr[ch * frames + f] += b->clientIn[ci][active][ch][f];
          }
        }
        ++nReady;
      }
    }
    if (nReady > 0) {
      int nextActive = b->mixedActive ? 0 : 1;
      for (int ch = 0; ch < 64; ++ch) {
        for (int f = 0; f < frames; ++f) {
          float s = sumPtr[ch * frames + f];
          b->mixedIn[nextActive][ch][f] = std::tanh(s);
        }
      }
      b->mixedActive = nextActive;
      for (int ci = 0; ci < 4; ++ci) b->ready[ci] = 0;
    } else {
      for (int ch = 0; ch < 64; ++ch) for (int f = 0; f < frames; ++f) b->mixedIn[b->mixedActive][ch][f] = 0;
    }
    // k-th Master IN slot of type BRIDGE(bi) <- summed client output k;
    // k-th Master OUT slot of type BRIDGE(bi) -> input k of every client (broadcast).
    const WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bi);
    int k = 0;
    for (uint32_t ii = 0; ii < table_->masterInCount && k < static_cast<int>(kBridgeChannels); ++ii) {
      if (table_->masterIn[ii].type != want) continue;
      std::memcpy(masterAudio_ + (512 + ii) * 4096, b->mixedIn[b->mixedActive][k++], frames * sizeof(float));
    }
    k = 0;
    for (uint32_t oi = 0; oi < table_->masterOutCount && k < static_cast<int>(kBridgeChannels); ++oi) {
      if (table_->masterOut[oi].type != want) continue;
      for (int ci = 0; ci < static_cast<int>(kBridgeClients); ++ci)
        std::memcpy(b->clientOut[ci][0][k], masterAudio_ + oi * 4096, frames * sizeof(float));
      ++k;
    }
    // The Master Clock drives every Bridge client each tick (after clientOut is fresh), whether or not
    // a client was ready: a client that missed one tick must not wait for its fallback timeout.
    for (int ci = 0; ci < 4; ++ci)
      if (bridgeTicks_[bi][ci]) SetEvent(bridgeTicks_[bi][ci]);
  }
  // Virtual Cable: DeviceIoControl stub — SHM Out -> Ring Write, Ring Read -> SHM In
  for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
    if (table_->masterOut[oi].type == SLOT_VIRTUAL) {
      int cable = table_->masterOut[oi].srcChannel % 8;
      if (cable < 0) cable = 0;
      if (virtualRings_[cable]) {
        int frames = static_cast<int>(table_->general.asioBuffer);
        if (frames > 4096) frames = 4096;
        float* outBuf = masterAudio_ + oi * 4096;
        // Interleave mono to stereo for ring
        float stereo[256 * 2] = {};
        for (int f = 0; f < frames && f < 256; ++f) stereo[f * 2] = stereo[f * 2 + 1] = outBuf[f];
        virtualRings_[cable]->write(stereo, frames);
      }
    }
  }
  for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
    if (table_->masterIn[ii].type == SLOT_VIRTUAL) {
      int cable = table_->masterIn[ii].srcChannel % 8;
      if (cable < 0) cable = 0;
      if (virtualRings_[cable] && virtualRings_[cable]->size() >= (uint32_t)table_->general.asioBuffer) {
        int frames = static_cast<int>(table_->general.asioBuffer);
        if (frames > 4096) frames = 4096;
        float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;
        float stereo[256 * 2] = {};
        if (virtualRings_[cable]->read(stereo, frames)) {
          for (int f = 0; f < frames; ++f) inBuf[f] = stereo[f * 2];
        }
      }
    }
  }
  // Network Streams: OUT slots -> Tx rings, Rx rings (jitter-primed) -> IN slots
  if (network_) {
    uint32_t frames = table_->general.asioBuffer;
    if (frames > 4096) frames = 4096;
    network_->processTick(masterAudio_, masterAudio_ + 512 * 4096, 4096, frames);
  }
  int hwFrames = static_cast<int>(table_->general.asioBuffer);
  if (hwFrames > 4096) hwFrames = 4096;
  // KS write: every HW OUT slot summed into its source device channel ("Ch N" = srcChannel N-1).
  if (ksAudio_ && ksAudio_->opened()) {
    std::memset(hwOut_, 0, sizeof(float) * kKsDeviceChannels * static_cast<size_t>(hwFrames));
    for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
      const auto& slot = table_->masterOut[oi];
      if (slot.type != SLOT_HW || slot.srcChannel < 0 || slot.srcChannel >= kKsDeviceChannels) continue;
      const float* outBuf = masterAudio_ + oi * 4096;
      float* dst = hwOut_ + static_cast<size_t>(slot.srcChannel) * hwFrames;
      for (int f = 0; f < hwFrames; ++f) dst[f] += outBuf[f];
    }
    ksAudio_->write(hwOut_, hwFrames, kKsDeviceChannels);
  }
  // KS read: HW -> each HW IN slot from its source device channel; the DAW gets it next tick.
  if (ksCapture_ && ksCapture_->opened()) {
    ksCapture_->read(hwIn_, hwFrames, kKsDeviceChannels);
    for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
      const auto& slot = table_->masterIn[ii];
      if (slot.type != SLOT_HW) continue;
      float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;
      if (slot.srcChannel >= 0 && slot.srcChannel < kKsDeviceChannels)
        std::memcpy(inBuf, hwIn_ + static_cast<size_t>(slot.srcChannel) * hwFrames, sizeof(float) * hwFrames);
      else
        std::memset(inBuf, 0, sizeof(float) * hwFrames);
    }
  }
}

}  // namespace wha
