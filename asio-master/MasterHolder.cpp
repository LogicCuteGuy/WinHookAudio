#include "MasterHolder.h"
#include <cmath>
#include <cstring>

namespace wha {

MasterHolder::MasterHolder(WHASlotTable* table, float* masterAudio, WHABridgeShared* bridges[4],
                           HANDLE masterTick, HANDLE tableChanged, HANDLE bridgeTicks[4][4])
    : table_(table), masterAudio_(masterAudio), masterTick_(masterTick), tableChanged_(tableChanged) {
  for (int i = 0; i < 4; ++i) bridges_[i] = bridges[i];
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) bridgeTicks_[i][j] = bridgeTicks[i][j];
}
MasterHolder::~MasterHolder() { stop(); }

bool MasterHolder::start() {
  if (running_) return true;
  stopRequested_ = false;
  running_ = true;
  DWORD id = 0;
  thread_ = CreateThread(nullptr, 0, threadProc, this, 0, &id);
  if (!thread_) { running_ = false; return false; }
  // MMCSS Pro Audio
  HMODULE avrt = LoadLibraryA("avrt.dll");
  if (avrt) {
    using AvSetMmThreadCharacteristicsA = HANDLE(WINAPI*)(LPCSTR, LPDWORD);
    auto pfn = reinterpret_cast<AvSetMmThreadCharacteristicsA>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsA"));
    if (pfn) { DWORD idx = 0; pfn("Pro Audio", &idx); }
  }
  return true;
}
void MasterHolder::stop() {
  if (!running_) return;
  stopRequested_ = true;
  if (masterTick_) SetEvent(masterTick_);
  if (thread_) { WaitForSingleObject(thread_, 1000); CloseHandle(thread_); thread_ = nullptr; }
  running_ = false;
}
DWORD WINAPI MasterHolder::threadProc(LPVOID param) { static_cast<MasterHolder*>(param)->run(); return 0; }
void MasterHolder::run() {
  HANDLE handles[2] = {masterTick_, tableChanged_};
  int nHandles = (masterTick_ && tableChanged_) ? 2 : (masterTick_ ? 1 : 0);
  while (!stopRequested_) {
    DWORD wait = (nHandles > 0) ? WaitForMultipleObjects(nHandles, handles, FALSE, INFINITE) : WaitForSingleObject(masterTick_, INFINITE);
    if (stopRequested_) break;
    if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) {
      doTick();
    }
  }
}
void MasterHolder::tickOnce() { doTick(); }

void MasterHolder::doTick() {
  if (!table_ || !masterAudio_) return;
  // Per-thing FIFOs: for now, just handle Loopback OUT->IN next tick and Bridge sum
  // Loopback: if masterOut[i].loopback && type==VIRTUAL, copy OUT->IN next tick
  // Master audio is float[2][512][4096] ping-pong; for 09 offline, simulate with masterAudio_ as flat
  // For offline test, masterAudio_ is 16MB = 2*512*4096*4; use active buffer 0
  // Loopback: copy OUT slot's audio to paired IN slot's audio (same Virtual index)
  for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
    const auto& outSlot = table_->masterOut[oi];
    if (outSlot.loopback && outSlot.type == SLOT_VIRTUAL) {
      // Find paired IN slot with same VIRTUAL srcChannel and loopback
      for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
        const auto& inSlot = table_->masterIn[ii];
        if (inSlot.loopback && inSlot.type == SLOT_VIRTUAL && inSlot.srcChannel == outSlot.srcChannel) {
          // Copy OUT->IN next tick: for offline, copy 128 frames (bufferSize) from OUT to IN
          // masterAudio layout: [2][512][4096] — use buffer 0, channel = slot index
          int frames = static_cast<int>(table_->general.asioBuffer);
          if (frames > 4096) frames = 4096;
          float* outBuf = masterAudio_ + oi * 4096;  // simplified: OUT channel oi
          float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;  // IN in second half
          // Actually ping-pong is [2][512][4096]; for test, just copy
          std::memcpy(inBuf, outBuf, frames * sizeof(float));
          break;
        }
      }
    }
  }
  // Shared Bridge sum: for each bridge, collect ready[0..3] with 3ms timeout, sum, tanh, broadcast
  for (int bi = 0; bi < 4; ++bi) {
    auto* b = bridges_[bi];
    if (!b) continue;
    // 3ms timeout per Bridge: check ready flags
    // For offline, just sum ready clients
    float* sum = new float[64 * 4096]();
    int nReady = 0;
    for (int ci = 0; ci < 4; ++ci) {
      if (b->ready[ci]) {
        // Sum clientIn[ci][active][ch][frame]
        int active = b->activeBuf[ci] ? 1 : 0;
        for (int ch = 0; ch < 64; ++ch) {
          for (int f = 0; f < 128; ++f) {  // 128 frames per tick
            sum[ch * 4096 + f] += b->clientIn[ci][active][ch][f];
          }
        }
        ++nReady;
      }
    }
    if (nReady > 0) {
      for (int ch = 0; ch < 64; ++ch) {
        for (int f = 0; f < 128; ++f) {
          float s = sum[ch * 4096 + f];
          b->mixedIn[0][ch][f] = std::tanh(s);
          b->mixedIn[1][ch][f] = std::tanh(s);
        }
      }
      b->mixedActive = 0;
      // Broadcast to 4 clients
      for (int ci = 0; ci < 4; ++ci) {
        if (bridgeTicks_[bi][ci]) SetEvent(bridgeTicks_[bi][ci]);
      }
    } else {
      // No ready clients: silence
      for (int ch = 0; ch < 64; ++ch) for (int f = 0; f < 128; ++f) b->mixedIn[0][ch][f] = 0;
    }
    delete[] sum;
  }
}

}  // namespace wha
