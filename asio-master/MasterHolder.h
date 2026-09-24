#pragma once

// MasterHolder — Worker MMCSS thread for 09.
// Vocabulary: Worker, Master Clock, Loopback, Shared Bridge.

#include <windows.h>

#include <atomic>
#include <functional>
#include <vector>
#include "WHASlotTable.h"
#include "WHASharedMemory.h"
#include "WHABridgeShared.h"

struct WHACableExchange;  // virtual/WHACableProtocol.h

namespace wha {

class MasterHolder {
 public:
  MasterHolder(WHASlotTable* table, float* masterAudio, WHABridgeShared* bridges[4],
               HANDLE masterTick, HANDLE tableChanged, HANDLE bridgeTicks[4][4]);
  ~MasterHolder();

  bool start();
  void stop();
  bool running() const { return running_; }
  // Master Clock side: wait (bounded) until the Worker has routed Master Clock tick `tick` (the
  // clock's tick count after that tick), so the next bufferSwitch reads its IN slots, no OUT slot is
  // overwritten before it was routed, and its HW output block is written. The event only wakes the
  // wait; the count decides. (Waiting for the event alone went one tick out of step for good after a
  // single timeout: the Worker's late signal for that tick then passed for the next one.)
  bool waitWorker(uint64_t tick, DWORD timeoutMs) const {
    if (!running_) return true;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    while (routed_.load() < tick) {
      QueryPerformanceCounter(&now);
      const double elapsedMs = 1000.0 * static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
      if (elapsedMs >= timeoutMs) return false;
      WaitForSingleObject(workerDone_, static_cast<DWORD>(timeoutMs - elapsedMs) + 1);
    }
    return true;
  }

  // HW devices by index (WHAHwMore: 0 = GENERAL's, 1..3 more). The Worker opens, at start, every
  // device a HW slot uses, and output device 0 whenever there is any HW slot: it paces the Master
  // Clock. Outputs 1..3 run on their own clocks through an HwOutputFifo each; every input has its
  // own HwInputFifo (KsCapture). A device listed twice (e.g. "Windows default" and the same device by
  // name) is opened once: the later index plays or records through the earlier one (alias).
  // The open, started HW output that paces the Master Clock; nullptr while none (internal timeline).
  class KsAudio* hwMaster() const { return hwOut_[0].load(); }
  // The open, started output / input device `d` (nullptr: not used, failed, or an alias).
  class KsAudio* hwOutput(int d) const { return d >= 0 && d < kHwDevices ? hwOut_[d].load() : nullptr; }
  class KsCapture* hwCapture(int d = 0) const { return d >= 0 && d < kHwDevices ? hwIn_[d].load() : nullptr; }
  // Output device d's FIFO (d >= 1; valid while hwOutput(d) is open).
  const class HwOutputFifo* hwOutputFifo(int d) const { return d >= 1 && d < kHwDevices ? outFifo_[d] : nullptr; }
  // HRESULT of the last failed open of device d (0 if none), for WHAMasterStats.
  int32_t hwOpenError(int d = 0) const { return d >= 0 && d < kHwDevices ? hwOutError_[d].load() : 0; }
  int32_t hwCaptureError(int d = 0) const { return d >= 0 && d < kHwDevices ? hwInError_[d].load() : 0; }
  // Device d is used by a HW slot (or is the Master Clock's): the Worker opened it or tried to.
  bool hwOutputUsed(int d) const { return d >= 0 && d < kHwDevices && outUsed_[d].load(); }
  bool hwInputUsed(int d) const { return d >= 0 && d < kHwDevices && inUsed_[d].load(); }
  // The device index d plays / records through (d itself, an earlier alias, or -1: not open).
  int hwOutputRoute(int d) const { return d >= 0 && d < kHwDevices ? outRoutePub_[d].load() : -1; }
  int hwInputRoute(int d) const { return d >= 0 && d < kHwDevices ? inRoutePub_[d].load() : -1; }
  // GENERAL and the more-devices list as the Worker read them to open the HW; false before that.
  bool hwRequest(WHAGeneral& out, WHAHwMore* more = nullptr) const {
    if (!hwRequestValid_.load(std::memory_order_acquire)) return false;
    out = hwRequest_;
    if (more) *more = hwRequestMore_;
    return true;
  }

  // Virtual Cable driver (WinHookAudio.sys): the Worker opens its control device at start. A cable
  // the driver has exchanges with its Windows endpoints each tick (DAW OUT -> Windows recording,
  // Windows playback -> DAW IN); other cables, or all without the driver, loop inside the Worker.
  int cableDriverCables() const { return cableCountPub_.load(); }  // 0 = not open
  int cableDriverError() const { return cableError_.load(); }      // Win32 error of the open, 0 if open
  // Cable c's last exchange (rates, queues, driver counters) and the Worker's counts; false if the
  // Worker has not exchanged it.
  bool cableStatus(int c, WHACableExchange& reply, uint64_t& exchanges, uint64_t& errors) const;

  // The Master Clock's tick count: the Worker compares it with its own to find ticks it missed (an
  // auto-reset Master_Tick coalesces them) and keeps the HW input in step.
  void setTickCounter(const std::atomic<uint64_t>* ticks) {
    clockTicks_ = ticks;
    routed_ = ticks ? ticks->load() : 0;  // ticks before this Worker (an earlier start) count as routed
  }

  // For testing: run one tick synchronously (no thread)
  void tickOnce();
  // Called on the Worker thread whenever TableChanged fires (any process may have saved).
  void setTableChangedHandler(std::function<void()> handler) { onTableChanged_ = std::move(handler); }

 private:
  static DWORD WINAPI threadProc(LPVOID param);
  void run();
  void openHw();  // Worker thread, at start
  void closeHw();
  void openCables();  // Worker thread, at start
  void closeCables();
  bool exchangeCable(int cable, uint32_t frames, bool hasRecord);  // virtualScratch_ <-> cableIo_
  void doTick();

  WHASlotTable* table_ = nullptr;
  float* masterAudio_ = nullptr;
  WHABridgeShared* bridges_[4] = {};
  HANDLE masterTick_ = nullptr;
  HANDLE tableChanged_ = nullptr;
  HANDLE workerDone_ = nullptr;  // auto-reset, set after each Master_Tick doTick (wakes waitWorker)
  std::atomic<uint64_t> routed_{0};  // the Master Clock tick count the Worker has routed up to
  HANDLE bridgeTicks_[4][4] = {};
  HANDLE thread_ = nullptr;
  bool running_ = false;
  bool stopRequested_ = false;
  HANDLE mmcssHandle_ = nullptr;
  HMODULE avrtModule_ = nullptr;
  const std::atomic<uint64_t>* clockTicks_ = nullptr;
  uint64_t ticksSeen_ = 0;
  bool haveTicks_ = false;
  // HW devices (see hwOutput). out_/in_/outFifo_ live as long as this object; the atomics publish
  // what is open to other threads (stats, the Master Clock).
  class KsAudio* out_[kHwDevices] = {};
  class HwOutputFifo* outFifo_[kHwDevices] = {};  // [0] unused: output 0 is the Master Clock's own
  class KsCapture* in_[kHwDevices] = {};
  int outRoute_[kHwDevices] = {-1, -1, -1, -1};  // Worker thread: device index -> open device, -1 none
  int inRoute_[kHwDevices] = {-1, -1, -1, -1};
  std::atomic<class KsAudio*> hwOut_[kHwDevices] = {};
  std::atomic<class KsCapture*> hwIn_[kHwDevices] = {};
  std::atomic<int32_t> hwOutError_[kHwDevices] = {};
  std::atomic<int32_t> hwInError_[kHwDevices] = {};
  std::atomic<bool> outUsed_[kHwDevices] = {};
  std::atomic<bool> inUsed_[kHwDevices] = {};
  std::atomic<int> outRoutePub_[kHwDevices] = {};
  std::atomic<int> inRoutePub_[kHwDevices] = {};
  WHAGeneral hwRequest_{};  // written once by the Worker before hwRequestValid_
  WHAHwMore hwRequestMore_{};
  std::atomic<bool> hwRequestValid_{false};
  float hwOutBuf_[kHwDevices][2 * 4096] = {};  // planar device-channel scratch per output (Worker thread)
  float hwInBuf_[kHwDevices][2 * 4096] = {};
  std::vector<float> outFrames_;  // interleaved frames for an output FIFO's device write (Worker thread)
  // Development trace (WINHOOKAUDIO_OUT_TRACE=<csv path>): every tick of output device #2, written at
  // stop. Preallocated: no allocation on the Worker while streaming.
  struct OutTraceRow { double sec; int64_t written; int32_t padding; int32_t wrote; int32_t backlog; double ratio; };
  std::vector<OutTraceRow> outTrace_;
  char outTracePath_[260] = {};
  float virtualScratch_[2 * 4096] = {};  // one Virtual Cable's interleaved stereo block (Worker thread)
  class WHARingBuffer* virtualRings_[8] = {};
  // Virtual Cable driver (Worker thread): control device, cables it has, exchange buffer (header +
  // WHA_CABLE_MAX_FRAMES stereo frames, allocated at start). cableStat_ publishes each cable's last
  // exchange to other threads (stats); fields are read one by one, not as a snapshot.
  HANDLE cableDevice_ = INVALID_HANDLE_VALUE;
  int cableCount_ = 0;
  std::atomic<int> cableCountPub_{0};
  std::atomic<int> cableError_{0};
  std::vector<unsigned char> cableIo_;
  struct CableStat {
    std::atomic<uint32_t> playRate{0}, recordRate{0}, playFill{0}, recordFill{0};
    std::atomic<uint32_t> playUnderruns{0}, playDrops{0}, recordUnderruns{0}, recordDrops{0};
    std::atomic<uint64_t> exchanges{0}, errors{0};
  };
  CableStat cableStat_[8];
  class WHANetworkEngine* network_ = nullptr;
  std::function<void()> onTableChanged_;  // WHAA Tx/Rx; its own thread does sockets + codecs
};

}  // namespace wha
