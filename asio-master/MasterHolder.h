#pragma once

// MasterHolder — Worker MMCSS thread for 09.
// Vocabulary: Worker, Master Clock, Loopback, Shared Bridge.

#include <windows.h>

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
  HANDLE bridgeTicks_[4][4] = {};
  HANDLE thread_ = nullptr;
  bool running_ = false;
  bool stopRequested_ = false;
  HANDLE mmcssHandle_ = nullptr;
  HMODULE avrtModule_ = nullptr;
  class KsAudio* ksAudio_ = nullptr;
  class WHARingBuffer* virtualRings_[8] = {};
  class WHANetworkEngine* network_ = nullptr;
  std::function<void()> onTableChanged_;  // WHAA Tx/Rx; its own thread does sockets + codecs
};

}  // namespace wha
