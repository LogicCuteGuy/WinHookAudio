# Spec — v10.1 ABI First Slice (Offline)

## Problem Statement
From the user's perspective: templates shift when channels are added or removed, Slave DAWs cannot share channels, VRChat to VRCT monitoring needs an extra track, and network use needs both low-latency PCM and compressed Vorbis. The current 32-channel proposal does not hold 512-slot templates stable.

## Solution
From the user's perspective: a stable 512 In + 512 Out Slot pool with separate INPUTS and OUTPUTS indices, Shared Bridge summing, opt-in Virtual Loopback, WHAA Network Streams, and a five-tab Control Panel. The first slice locks the Slot Table, shared-memory names, and persistent JSON schema offline, with no audio claim.

Seam: single shared ABI definition validated offline via compile-time checks and command-line tests, reusing the existing offline CLI test pattern. No device or audio seam in this slice.

## User Stories
1. As a Master DAW user, I want 512 input Slots with stable indices, so that drum, bass, guitar, and vocal template positions never shift.
2. As a Master DAW user, I want 512 output Slots independent from inputs, so that reordering inputs does not move outputs.
3. As a Master DAW user, I want empty Slots that count but stay silent, so that separators preserve numbering.
4. As a Master DAW user, I want custom 32-character Slot names, so that tracks show SM58 Mic, Discord, and To VRCT.
5. As a Master DAW user, I want dynamic counts 1..512, so that unused channels stay hidden until needed.
6. As a Slave DAW user, I want a Bridge Driver exposing only its Bridge subset, so that FL Studio sees 16 channels while the Master DAW sees 512.
7. As a Slave DAW user, I want four Slave DAWs sharing one Bridge channel set, so that FL Studio and Live jam on the same sum.
8. As a Slave DAW user, I want summed Bridge input with soft-clip, so that two DAWs at -6 dB arrive loud, not quiet.
9. As a Slave DAW user, I want broadcast Bridge output, so that all four clients hear the same Master DAW return.
10. As a streamer, I want per-Virtual Slot Loopback flags, so that VRChat audio goes through the Master DAW to VRCT without an extra track.
11. As a streamer, I want Loopback isolated by default, so that Chrome, Discord, and OBS paths do not feed back unexpectedly.
12. As a streamer, I want 8x stereo Virtual Cables, so that VRChat, Discord, Chrome, and OBS each have a dedicated path.
13. As a network user, I want addressed Tx and Rx Network Streams with per-stream codec, so that a laptop can send Vorbis while studio B sends PCM.
14. As a network user, I want PCM for LAN and Vorbis with quality 0.1..1.0 for WAN, so that 64 channels fit WAN bandwidth.
15. As a network user, I want jitter targets per codec, so that PCM stays tight and Vorbis stays stable.
16. As a performer, I want one Master Clock for rate and buffer size, so that all 512 Slots move on one tick.
17. As a performer, I want per-thing Worker FIFOs for hardware, virtual, bridge, and network, so that guitar stays low-latency while network stays buffered.
18. As a performer, I want closing the Master DAW to equal silence, so that no stale audio replays.
19. As a template keeper, I want insert-empty and drag-reorder on inputs alone, so that adding a bass mic does not shift guitars.
20. As a template keeper, I want persistent slots file load on init and save on panel Save, so that reboot restores the full map.
21. As a panel user, I want INPUTS, OUTPUTS, NETWORK, GENERAL, and ABOUT tabs, so that 512 rows stay filterable.
22. As a panel user, I want Bridge GENERAL read-only following Master, so that Slave DAWs cannot split the clock.
23. As a film scorer, I want 8 Tx plus 8 Rx streams carrying up to 512 Vorbis channels, so that orchestra stems cross WAN.
24. As a producer, I want four Bridges each with four clients, so that 16 Slave DAWs share one Master DAW.

## Implementation Decisions
- Shared ABI module defines the Slot Table version, 512 maximum, 4 Bridge count, 32-character names, and 8 network streams.
- Slot types are none, hardware, virtual, network, and Bridge 1..4 as types inside the pool, not extra pool.
- Slot record carries type, source channel, stream identifier for network only, name, enabled flag, loopback flag valid only for virtual, plus padding for stable size.
- Global settings carry Master Clock rate, bit depth fixed to float32, ASIO buffer size, plus per-thing Worker FIFOs for hardware, virtual, four Bridges, PCM network, Vorbis network, and jitter targets.
- Network Stream record carries address text, port default 6980, codec choice PCM float32 / PCM int16 / Vorbis, quality, and channel count with PCM-safe and Vorbis-compressed ranges.
- Slot Table record carries version increment on Save, input count, input array, output count, output array, global settings, 8 Tx streams, 8 Rx streams.
- Invariants: Bridge types inside 512; none-type counts but silent; loopback meaningful only on virtual; input index independent from output index.
- Control-plane size fixed at 80 KB class; data-plane sizes fixed at 16 MB Master audio ping-pong plus 8 MB per Bridge shared region; total pre-allocated on Master init.
- Shared-memory naming covers Slot Table, Master audio, four Bridge shared regions, Master tick event, 16 Bridge tick events, and TableChanged event.
- Bridge shared region carries client count 0..4, per-client ready flags, per-client active buffer flags, per-client input banks, per-client output banks, mixed sum bank, and mixed active flag.
- Audio packet header carries WHAA magic version 5, codec, channels, frames, stream identifier, sequence, clock stamp, and payload length; Vorbis handshake carries codebook headers sent redundantly on Save.
- Version change triggers host reset request so the Master DAW re-queries counts and names; per-thing FIFO change reloads Worker without host reset.
- First slice is header and schema only; no ASIO entry points, no Worker, no kernel driver, no socket I/O, no GUI toolkit in this slice.
- C++20 for user-mode definitions; WDK-compatible C for any kernel-shared constants; fixed-width integer types; no pointers across the ABI boundary.

## Testing Decisions
- Good tests check external behavior only: sizes, offsets, version bump, name truncation, count bounds, JSON round-trip, and name mapping, not internal layout tricks.
- Modules tested: Slot Table definition, shared-memory name set, persistent JSON schema mapping.
- Prior art: existing offline CLI tests that assert help output and reject malformed arguments without touching devices; extend that pattern with compile-time size assertions and offline schema validation.
- All tests run without audio hardware, without DAW, without driver install, without network sockets, without admin rights.
- Failure cases: out-of-range counts, overlong names, invalid codec, invalid stream identifier, loopback on non-virtual, fifth Bridge client rejected.

## Out of Scope
- Master Driver streaming, bufferSwitch memcpy, Worker thread, MMCSS scheduling, resampling, and latency measurement.
- Bridge Driver subset enumeration, client identity allocation, sum and broadcast data movement.
- Virtual Cable kernel driver, rings, and device control codes.
- WHAA encode and decode, jitter buffers, discovery broadcasts, firewall rules.
- Control Panel rendering, ImGui, DX11 popup thread, filtering, drag-reorder visuals.
- Installer, registry keys, signing, packaging, DAW compatibility matrix.
- Use-case template imports Plan A through Plan E beyond schema capacity to hold them.

## Further Notes
- Vocabulary follows the project glossary: Master DAW, Slave DAW, Master Driver, Bridge Driver, Virtual Cable, Slot, Slot Table, Master Clock, Loopback, Shared Bridge, Network Stream, Control Panel.
- Respects ADRs 0001 through 0006: 512 pool, pure DLL Worker, WHAA only, shared sum, opt-in loopback, five-tab panel with header-only first slice.
- v10.1 Data Architecture governs over v10 summary and over earlier 32-channel stages where they conflict; architecture and plan docs to be updated separately.
- No audio functionality claimed without runtime verification.
