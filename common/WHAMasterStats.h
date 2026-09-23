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
  int32_t hwFillAtTick;     // mean device fill when the HW Master Clock ticked, frames
  int32_t hwStreamLatency;  // device stream latency, frames
  // HW input (capture) into HW IN slots.
  int32_t hwInOpen;         // HW input open and started
  int32_t hwInLastError;    // HRESULT of the last failed HW input open, 0 if none
  uint64_t hwInReads;       // blocks delivered to HW IN slots
  uint64_t hwInStarved;     // ticks with no block ready (silence, a larger target, re-prime)
  uint64_t hwInTrims;       // FIFO overflowed or far above target
  uint64_t hwInGlitches;    // device-flagged discontinuities
  int32_t hwInFill;         // backlog (queued + captured, not yet delivered) at the last read
  int32_t hwInTarget;       // backlog target at a read, frames
  int32_t hwInMeanFill;     // mean backlog at a read
  int32_t hwInStreamLatency;  // capture device stream latency, frames (informational)
  int32_t hwInDriftPpmMilli;  // capture clock vs Master Clock, ppm x 1000 (+ = device fast)
  int32_t hwInDriftEngaged;   // drift resampling active
  uint64_t hwInGrowths;       // backlog target grew after a starve (delivery jitter)
  uint64_t hwInSkipped;       // frames dropped for Master Clock ticks the Worker missed
};

// Returns 0 on success, -1 when no Master instance is streaming in this process.
using WHAGetMasterStatsFn = int(__stdcall*)(WHAMasterStats*);

}  // namespace wha
