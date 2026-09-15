#pragma once

// MasterHolder — Worker MMCSS thread for 09.
// Vocabulary: Worker, Master Clock, Loopback, Shared Bridge.

#include <windows.h>
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
};

}  // namespace wha
