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

// Exclusive sample format a HW device accepted (KsSampleFormat order).
enum WHAHwFormat : int32_t {
  HW_FORMAT_NONE = -1,
  HW_FORMAT_FLOAT32 = 0,
  HW_FORMAT_PCM24IN32 = 1,  // 24 valid bits in a 32-bit container
  HW_FORMAT_PCM16 = 2,
};

constexpr int kStatsEndpointIdLen = 64;  // = kEndpointIdLen (WHASlotTable.h)

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
  // Requested versus actual (Control Panel GENERAL). Requested = the Slot Table's GENERAL when the
  // Worker opened the HW; a later Save applies only after the DAW resets the driver.
  int32_t sampleRate;         // Master Clock
  int32_t asioBuffer;         // Master Clock block, frames
  int32_t hwRequestValid;     // the hwRequested* fields below are filled
  int32_t hwRequestedPeriod;  // GENERAL hwBuffer at open (0 = Auto: device minimum)
  char hwRequestedRenderId[kStatsEndpointIdLen];   // "" = Windows default device
  char hwRequestedCaptureId[kStatsEndpointIdLen];
  char hwRenderId[kStatsEndpointIdLen];            // endpoint actually open ("" while none)
  char hwCaptureId[kStatsEndpointIdLen];
  int32_t hwPeriod;           // actual device period, frames
  int32_t hwFormat;           // WHAHwFormat
  int32_t hwLatency;          // output latency reported to the DAW, frames
  int32_t hwInPeriod;
  int32_t hwInFormat;
  int32_t hwInLatency;        // input latency reported to the DAW, frames
};

// Returns 0 on success, -1 when no Master instance is streaming in this process.
using WHAGetMasterStatsFn = int(__stdcall*)(WHAMasterStats*);

}  // namespace wha
