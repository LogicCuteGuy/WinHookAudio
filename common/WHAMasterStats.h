#pragma once

// WHAMasterStats — counters a host can read from the loaded Master DLL while it streams:
//   auto get = (WHAGetMasterStatsFn)GetProcAddress(masterDll, "WHAGetMasterStats");
// Vocabulary: Master Clock, Worker.

#include <cstdint>

namespace wha {

enum WHAClockSource : int32_t {
  CLOCK_INTERNAL = 0,  // QPC-paced internal timeline (no HW output open)
  CLOCK_HARDWARE = 1,  // paced by the HW output's buffer drain
};

struct WHAMasterStats {
  uint64_t ticks;           // Master Clock bufferSwitch count
  uint64_t clockOverruns;   // internal timeline resyncs (stall > 8 periods; shorter ones catch up)
  uint64_t workerOverruns;  // ticks the Worker had not routed within one period
  uint64_t hwWrites;        // blocks written to the HW output
  uint64_t hwUnderruns;     // writes that found the device buffer already empty (audible gap)
  uint64_t hwDrops;         // blocks dropped: device buffer had no room
  int32_t clockSource;      // WHAClockSource of the last tick
  int32_t hwOpen;           // HW output open and started
  int32_t hwLastError;      // HRESULT of the last failed HW open, 0 if none
  int32_t hwCapacity;       // device buffer, frames
  int32_t hwMinFill;        // lowest device fill seen at a write, frames (-1: no writes)
  int32_t hwMaxFill;        // highest device fill seen at a write, frames
};

// Returns 0 on success, -1 when no Master instance is streaming in this process.
using WHAGetMasterStatsFn = int(__stdcall*)(WHAMasterStats*);

}  // namespace wha
