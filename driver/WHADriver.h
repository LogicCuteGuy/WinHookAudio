#pragma once

// WinHookAudio.sys - the Virtual Cable kernel driver (PortCls WaveRT, root-enumerated).
// Each cable exposes a Windows playback and a recording endpoint; the Worker moves their audio to and
// from the DAW through the control device (common/virtual/WHACableProtocol.h).
// Vocabulary: Virtual Cable, Worker.

#define _NEW_DELETE_OPERATORS_  // stdunk.h's inline operator new is deprecated; ours are in WHAAdapter.cpp
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>

#include "../common/virtual/WHACableFormat.h"
#include "../common/virtual/WHACableRing.h"

// Kernel C++ allocation (WHAAdapter.cpp): ExAllocatePool2, so zeroed.
void* __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag);
void* __cdecl operator new[](size_t size, POOL_FLAGS flags, ULONG tag);
void __cdecl operator delete(void* p, POOL_FLAGS flags, ULONG tag);

namespace wha {

constexpr ULONG kPoolTag = 'cAHW';
constexpr ULONG kCables = 8;  // = kVirtualSlotCables (WHASlotTable.h)
constexpr ULONG kSubdevicesPerCable = 4;  // wave + topology, for playback and for recording

// One side's Windows volume and mute (the topology filter's volume and mute nodes), per channel.
// The stream applies `gain` (0 when muted); Windows does not, since the endpoint has its own nodes.
struct WHACableLevel {
  static constexpr ULONG kChannels = kCableChannels;
  LONG volume[kChannels] = {};  // 1/65536 dB, kVolumeMin..0
  BOOL mute[kChannels] = {};
  float gain[kChannels] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};

// The one format a cable's endpoints offer (both sides): the Master's rate, the cable's channels and
// sample format, as the Worker last sent them (WHACableExchange). Until a Worker connects: 48 kHz
// stereo float.
struct WHACableFormat {
  ULONG rate = 48000;
  ULONG channels = 2;
  WHASampleKind kind = WHASampleKind::Float32;
};

// One Virtual Cable. `lock` guards everything here and the state of the streams on it.
struct WHACable {
  KSPIN_LOCK lock;
  WHACableRing play;    // Windows apps' playback -> Worker (a DAW input)
  WHACableRing record;  // Worker (a DAW output) -> Windows apps' recording
  ULONG playRate;       // Hz of the running playback stream, 0 = none
  ULONG recordRate;
  ULONG workerFrames;   // frames per Worker exchange (the last one): sets the rings' latency
  WHACableLevel playLevel, recordLevel;
  WHACableFormat format;
  // Both wave filters' streaming pins list this one data range (the format above; SetCableFormatLocked
  // rewrites it in place, PortCls reads it through the pointer on each request).
  KSDATARANGE_AUDIO range;
  PKSDATARANGE ranges[1];
  // Each wave filter's port events (null while it is not registered), to tell Windows the format changed.
  PPORTEVENTS events[2];  // [0] playback (render), [1] recording (capture)
};

extern WHACable* g_cables;  // kCables entries, nonpaged; lives as long as the driver

// Sets cable `c`'s format and its data range (c.lock held). False if nothing changed.
bool SetCableFormatLocked(WHACable& c, const WHACableFormat& format);
// Tells Windows that cable `c`'s formats changed (both endpoints); PASSIVE_LEVEL, lock not held.
void NotifyCableFormatChange(WHACable& c);

// The ring latency for one side: two blocks of the side that delivers in the largest steps (the
// Worker's block; a WaveRT stream is copied every millisecond).
inline unsigned CablePrime(const WHACable& c) { return 2 * (c.workerFrames ? c.workerFrames : 512); }
inline unsigned CableSlack(const WHACable& c) { return 4 * (c.workerFrames ? c.workerFrames : 512); }
// Adaptive latency (WHACableRing::read): each underrun on a side adds one Worker block to its prime,
// up to 50 ms or 4 blocks, whichever is more (at most half the ring). Only one block of prime is
// margin (the other is the block in flight), so a Worker tick later than one block underruns: on a
// busy PC or a VM the cable settles at the latency it needs. Reset when a Worker opens the control
// device or changes its block.
inline unsigned CableGrow(const WHACable& c) { return c.workerFrames ? c.workerFrames : 512; }
inline unsigned CableMaxPrime(const WHACable& c) {
  unsigned most = c.format.rate / 20;
  if (most < 4 * CableGrow(c)) most = 4 * CableGrow(c);
  if (most > WHACableRing::kFrames / 2) most = WHACableRing::kFrames / 2;
  return most;
}

// Miniports (WHAMiniports.cpp). `capture`: the cable's recording side.
NTSTATUS NewWaveMiniport(PUNKNOWN* out, ULONG cable, bool capture);
NTSTATUS NewTopologyMiniport(PUNKNOWN* out, ULONG cable, bool capture);

// Pin numbers, as the adapter connects the filters.
constexpr ULONG kWaveRenderStreamPin = 0, kWaveRenderBridgePin = 1;
constexpr ULONG kWaveCaptureBridgePin = 0, kWaveCaptureStreamPin = 1;
constexpr ULONG kTopoRenderWavePin = 0, kTopoRenderSpeakerPin = 1;
constexpr ULONG kTopoCaptureMicPin = 0, kTopoCaptureWavePin = 1;

}  // namespace wha
