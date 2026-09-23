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
constexpr int kStatsHwMore = 3;          // = kHwDevices - 1 (WHASlotTable.h)

// One more HW device (index 1..3 of its direction).
struct WHAHwDeviceStats {
  char requestedId[kStatsEndpointIdLen];  // the list's ID when the Worker opened the HW ("" = not listed)
  char id[kStatsEndpointIdLen];           // endpoint open ("" while none)
  int32_t used;           // a HW slot uses it: the Worker opened it or tried to
  int32_t open;           // open and started
  int32_t sameAs;         // -1, or the device index it plays / records through (listed twice)
  int32_t lastError;      // HRESULT of a failed open, 0 if none
  int32_t period;         // device period, frames
  int32_t format;         // WHAHwFormat
  int32_t latency;        // frames from the DAW's block to the device (out) / device to the DAW (in)
  int32_t fill;           // backlog at the last tick, frames
  int32_t target;         // backlog target, frames
  int32_t driftPpmMilli;  // device clock vs Master Clock, ppm x 1000 (+ = device fast)
  int32_t driftEngaged;   // drift resampling active
  int32_t chunk;          // out: frames its position reports jump at once (0 = smooth)
  uint64_t blocks;        // blocks passed to / from it
  uint64_t underruns;     // out: device found empty; in: backlog ran empty
  uint64_t gaps;          // out: silence written to catch up; in: device-flagged discontinuities
  uint64_t trims;         // times frames were thrown away (backlog far above target / full)
  uint64_t growths;       // in: backlog target grew
  uint64_t skipped;       // in: frames dropped for Master Clock ticks the Worker missed
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
  // HW Master Clock pacing (HwClockPacer): ticks evenly spaced at the device's rate.
  int32_t hwChunk;            // frames the device's position reports jump at once (0 = smooth)
  uint64_t hwHurries;         // ticks taken at once because the device was nearly empty
  // HW devices 2..4 of each direction (WHAHwMore): each on its own clock, resampled to the Master Clock.
  WHAHwDeviceStats hwMoreOut[kStatsHwMore];
  WHAHwDeviceStats hwMoreIn[kStatsHwMore];
};

// Returns 0 on success, -1 when no Master instance is streaming in this process.
using WHAGetMasterStatsFn = int(__stdcall*)(WHAMasterStats*);

}  // namespace wha
