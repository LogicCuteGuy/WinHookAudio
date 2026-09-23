#pragma once

// MasterHolder — Worker MMCSS thread for 09.
// Vocabulary: Worker, Master Clock, Loopback, Shared Bridge.

#include <windows.h>

#include <atomic>
#include <functional>
#include "WHASlotTable.h"
#include "WHASharedMemory.h"
#include "WHABridgeShared.h"

namespace wha {

class MasterHolder {
 public:
  MasterHolder(WHASlotTable* table, float* masterAudio, WHABridgeShared* bridges[4],
               HANDLE masterTick, HANDLE tableChanged, HANDLE bridgeTicks[4][4]);
  ~MasterHolder();

  bool start();
  void stop();
  bool running() const { return running_; }
  // Master Clock side: wait (bounded) until the Worker has routed the last Master_Tick, so the next
  // bufferSwitch reads this tick's IN slots and no OUT slot is overwritten before it was routed.
  bool waitWorker(DWORD timeoutMs) const {
    return !running_ || WaitForSingleObject(workerDone_, timeoutMs) == WAIT_OBJECT_0;
  }

  // The open, started HW output that paces the Master Clock; nullptr while none (internal timeline).
  class KsAudio* hwMaster() const { return hwMaster_.load(); }
  // HRESULT of the last failed HW open (0 if none), for WHAMasterStats.
  int32_t hwOpenError() const { return hwOpenError_.load(); }

  // For testing: run one tick synchronously (no thread)
  void tickOnce();
  // Called on the Worker thread whenever TableChanged fires (any process may have saved).
  void setTableChangedHandler(std::function<void()> handler) { onTableChanged_ = std::move(handler); }

 private:
  static DWORD WINAPI threadProc(LPVOID param);
  void run();
  void doTick();

  WHASlotTable* table_ = nullptr;
  float* masterAudio_ = nullptr;
  WHABridgeShared* bridges_[4] = {};
  HANDLE masterTick_ = nullptr;
  HANDLE tableChanged_ = nullptr;
  HANDLE workerDone_ = nullptr;  // auto-reset, set after each Master_Tick doTick
  HANDLE bridgeTicks_[4][4] = {};
  HANDLE thread_ = nullptr;
  bool running_ = false;
  bool stopRequested_ = false;
  HANDLE mmcssHandle_ = nullptr;
  HMODULE avrtModule_ = nullptr;
  class KsAudio* ksAudio_ = nullptr;
  std::atomic<class KsAudio*> hwMaster_{nullptr};
  std::atomic<int32_t> hwOpenError_{0};
  class WHARingBuffer* virtualRings_[8] = {};
  class WHANetworkEngine* network_ = nullptr;
  std::function<void()> onTableChanged_;  // WHAA Tx/Rx; its own thread does sockets + codecs
};

}  // namespace wha
