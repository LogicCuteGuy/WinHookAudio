# Several HW devices per direction

Date: 2026-09-24
Status: accepted

## Context
HW slots used one output and one input device (GENERAL `hwRenderId` / `hwCaptureId`, stage 2 of the
implementation plan). Stage 3 asks for device aggregation. Every device has its own clock: equal
nominal rates drift apart, so a second output cannot be written block-for-block like the Master
Clock's device without running dry or overflowing. The input side already had a FIFO with drift
resampling per device (`HwInputFifo`). The Slot Table is shared memory read by other processes and
saved as `slots.json`; earlier fields must keep their offsets and older files must still load.

## Decision
- Up to 4 devices per direction. Device 0 stays GENERAL `hwRenderId` / `hwCaptureId`; devices 1..3
  are `WHAHwMore` appended after `netRx` in `WHASlotTable` (`hwRenderMore` / `hwCaptureMore` in
  `slots.json`, optional). A HW slot's `streamId` is its device index (unused for HW before, so
  older tables mean device 0).
- Output device 0 is the Master Clock (paced by `HwClockPacer` as before). Outputs 1..3 each run
  through an `HwOutputFifo`: the block is resampled (PI loop on the backlog = queue + device fill,
  as `HwInputFifo`) and written as the device has room. The device fill comes from a position
  tracker that only corrects when its timeline leaves the interval the device's (possibly stepped)
  report allows, so a device that reports in 512-frame steps does not beat against the ticks. Loop
  time scale 1 s (4 s for stepped devices) while it learns the drift, then 16 s: the pitch follows
  the average while the backlog absorbs a wandering Master Clock.
- The Worker opens, at start, every device a HW slot uses. Two list entries that resolve to the same
  endpoint (e.g. "Windows default" and the same device by name) open once; the later one plays or
  records through the earlier. A device change is DAW-visible (reset), like the one-device case.
- ASIO has one latency pair: output = the Master Clock's device, input = the lowest-numbered open
  input. Each device's own latency and clock drift are in the Control Panel (GENERAL "Actual").
- The Source menu picks a device by name: a listed one; otherwise it switches device 0 when no other
  slot uses it (the old one-device behaviour), else it is added to the list.

## Consequences
- Offline: `hw-out-fifo-test` (drift ±100..±5000 ppm, stepped and VMware-like devices, Worker jitter,
  stalls, missed ticks, a wandering Master Clock). Equal clocks stay bit-exact.
- Live on the dev VM (VMware HD Audio as clock, VB-Cable as device #2 both ways): works, resampled
  on each side; after ~20 s the output #2 ratio holds within ~±300 ppm. The first seconds show
  dropouts while the Master Clock warms up in bursts, and larger pitch correction while the loop
  learns; on this VM the Master Clock itself wanders by up to ~2500 ppm. Not heard by ear, not run on
  physical HW.
- Latency per extra output ≈ device buffer − one block (+ resampler + stream latency), e.g. 26 ms at
  HW 256 / 44.1 kHz.
- `WHAMasterStats` grows (append-only `hwMoreOut` / `hwMoreIn`).
