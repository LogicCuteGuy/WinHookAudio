# More Master Clock rates, 16 Virtual Cables

Date: 2026-09-25
Status: accepted

## Context
The Master Clock offered 44.1, 48 and 96 kHz, chosen only in the Control Panel: a DAW's own rate
menu listed just the current rate. The Virtual Cable driver had 8 cables.

## Decision
- Master Clock rates: 44.1, 48, 88.2, 96, 176.4 and 192 kHz (`kSampleRates`, `IsValidSampleRate` in
  `common/WHASlotTable.h`), for the panel, the Slot Table check, WHAA codebooks and Vorbis.
- The DAW's rate menu works: the Master's `canSampleRate` accepts every rate in the list, and
  `setSampleRate` writes it to the Slot Table (tells Bridges and the panel through the table-changed
  event). Stopped: it applies at the next start. Streaming: the Master asks the DAW for a reset. It is
  not saved to the config files; a Control Panel Save keeps it. Bridges still follow the Master.
- 16 Virtual Cables (`kVirtualSlotCables`): cables 9..16's settings are appended to the Slot Table
  (`cablesMore`, after `hwModes`) and their stats to `WHAMasterStats` (`cablesMore`), so every
  earlier field keeps its offset; read them through `CableSetting` / `CableStats`. Files from 8-cable
  versions load (cables 9..16 stereo float). The driver has 16 cables (32 Windows endpoints,
  `WinHookAudio Output/Input 9..16`, name GUIDs `5EDB54C8..CF` / `5EDB54D8..DF`); `CableEndpointSync`
  reads two-digit filter names (`outputwave16`).
- The Worker finds the cables in use with one pass over the slots per tick, so an unused cable costs
  nothing; a used one costs one driver exchange per tick.

## Consequences
- A cable's latency does not depend on how many cables run: each has its own ring (two Worker blocks
  plus what its underruns add, `driver/WHADriver.h`).
- Measured on the dev VM (internal Master Clock, `asio-live`, 10 s runs):

  | Rate / buffer | 1 cable | 16 cables (8 through the driver, 8 inside the Worker) |
  |---|---|---|
  | 48 kHz / 128 | 0 Worker overruns, cable 16 loop 128 frames steady, bit-exact | same |
  | 192 kHz / 512 | 0 Worker overruns | 0 Worker overruns (1 of 3 runs: 9 late callbacks, as 1-cable runs show a few) |
  | 192 kHz / 128 | 1500+ late callbacks | same, plus 1-3 Worker overruns |

  A 0.67 ms period (128 frames at 192 kHz) is too short for this VM with any number of cables; at
  high rates use a larger buffer (same milliseconds as at 48 kHz).
- Through the installed 8-cable driver (`cable-live`, Windows <-> cable): 88.2, 176.4 and 192 kHz pass
  (one 176.4 kHz run of four had a start-up underrun while the cable settled, as 48 kHz runs do).
- The 16-cable driver builds; it is not yet installed and checked live on a PC.
