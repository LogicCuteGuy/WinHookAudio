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
- Up to 128 devices per direction (was 4 until 2026-09-25: raised so every device a PC has can be
  assigned; 128 is what the fixed 80 KB Slot Table mapping comfortably holds at 128 bytes per
  device place, far more than a PC has, so no user meets the list's end). Device 0 stays GENERAL
  `hwRenderId` / `hwCaptureId`; devices 1..3 are `WHAHwMore` appended after `netRx`, devices 4..127
  `WHAHwExtra` appended after `cables` in `WHASlotTable`, so every earlier field keeps its offset
  (the table grows from ~50 KB to ~65 KB). A truly unbounded list would need the table out of its
  fixed shared-memory layout; not worth it for a limit nobody reaches. Code
  reads them only through `HwDeviceId`. `settings.yml` lists each direction up to its last listed
  device; `slots.json` (`hwRenderMore` / `hwCaptureMore`) takes any length, so older files with 3
  entries load. `WHAMasterStats` appends `hwExtraOut` / `hwExtraIn` (read with `HwDeviceStats`). The
  Worker allocates a device's scratch buffers when it opens it, so unused places cost nothing. A HW
  slot's `streamId` is its device index (unused for HW before, so older tables mean device 0).
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
- `WHAMasterStats` grows (append-only `hwMoreOut` / `hwMoreIn`, then `hwExtraOut` / `hwExtraIn`).
- Live on the dev VM with 8 output devices at once (HD Audio as clock, 2 VB-Cables, WinHookAudio
  Output 1..5; `asio-live`, 6 s, block 256 @ 48 kHz): all 8 open and stream, 0 underruns, 0 gaps on
  each. Each extra output trims ~12 times in the first seconds, the same count with 2 devices and on
  the code from before this change (the Master Clock warm-up above). Not heard by ear.
- Channels (2026-09-25, was stereo only): each device opens with all the channels of its Windows
  device format (`PKEY_AudioEngine_DeviceFormat`, read in `common/WHAEndpointChannels.h` by both the
  Worker and the Control Panel), with that format's speaker mask; if the device refuses that in
  exclusive mode, stereo as before. A HW slot's `srcChannel` is any channel up to 64 (`kHwMaxChannels`);
  one past what the device opened is silent. The Source menu lists every channel ("Ch 3" ...), and
  `WHAMasterStats` appends `hwOutChannels` / `hwInChannels` so GENERAL shows "16 ch".
  Live (VB-Audio "CABLE In 16ch" as output #2, 18 DAW outputs): opened with 16 channels, noise on its
  Ch 1 came back through CABLE Output. That run (Bitwig open, Master Clock 0.5-0.7 % off) had 13-15
  underruns on the 16-channel device and 0 on the stereo CABLE Input in the same test; the 16-channel
  device got half the buffer (target 896 vs 1792 frames). Not yet explained; re-measure on a quiet PC.
- Start-up dropouts (2026-09-25): the "dropouts while the Master Clock warms up" and "~12 trims per
  extra output" above had one cause. VMware HD Audio as the Master Clock's device takes frames at up to
  ~1.8x real time for its first ~0.4 s (~220 ms of audio extra); the pacer follows it on purpose, so
  every other device was read or fed at that pace: the input starved 5 times in 0.12-0.42 s (and each
  starve raised its backlog target for the whole session: input latency 52.7 ms instead of ~25 ms),
  an extra output overflowed ~12 times. Now the Worker holds the HW inputs and the more outputs in
  silence until the Master Clock's warm-up (1 s) is over (`MasterHolder::setClockSettled`,
  `KsCapture::hold`), then starts their FIFOs afresh, an output's primed with silence to its target
  (`HwOutputFifo::primeSilence`) so it joins the device's silence without a gap. Cost: those devices
  are silent for the first second after every start. Live (7 runs, 12 s, 5 set-ups): 0 dropout events,
  input latency 24-26 ms, "no gap after start" passes. One of 8 loop runs on VB-Cable stepped by 249
  frames at 2.5 s with no counter moving: VB-Cable's own, a plain WASAPI loop on it with no
  WinHookAudio driver steps the same way (0014).
- The Control Panel's thread gets its own 4 MB stack: the edit copy, baseline, Save snapshot and
  stats (~65 KB each) no longer depend on the DAW's default thread stack.
- Each open device is one more exclusive stream the Worker serves every tick; the practical limit is
  the PC, not the list.
