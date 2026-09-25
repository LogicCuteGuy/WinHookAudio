# HW device modes: Exclusive, Shared, Auto (no raw KS, no MME)

Date: 2026-09-25
Status: accepted

## Context
HW devices opened only in WASAPI Exclusive (0012). Such a device is the DAW's alone while it streams,
and it fails to open when Windows forbids exclusive mode for it or it has no exclusive format at the
Master Clock's rate. Other ASIO hosts offer "WDM", "KS" and "MME" as ways to reach a sound card:

- WDM is the driver model, not a way to open a device; every Windows sound driver is one. What such
  hosts call "WDM" is WASAPI.
- Raw Kernel Streaming (KSPROPERTY on the driver's pin) reaches the same WaveRT buffer WASAPI
  Exclusive maps; only old WaveCyclic drivers could go lower. Much code for no gain on current drivers.
- MME (waveOut / waveIn) goes through the Windows mixer with its own, larger buffering. WASAPI Shared
  does the same job with less latency.

## Decision
- Each HW device has a mode, per direction (`WHAHwMode`: Exclusive = 0, Shared = 1, Auto = 2), stored
  as `WHAHwModes` appended after `hwExtra` in `WHASlotTable`: older tables and files mean Exclusive,
  the behaviour before. `settings.yml` has `hwDevices: outputModes / inputModes` (words, same order as
  the device lists, optional); `slots.json` has `hwRenderModes` / `hwCaptureModes` (numbers, optional).
  A mode change is DAW-visible (reset), like a device change. The Control Panel's GENERAL device rows
  get a Mode box; "Actual" says which mode each device opened in (Auto: which one it got).
- **Exclusive**: unchanged (0012).
- **Shared** (`KsOpen`, `asio-master/KsEndpoint.cpp`): the mixer's channels, float, at the Master
  Clock's rate with `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` (Windows converts when the mixer runs at
  another rate), timer-driven like Exclusive. The period is the mixer's (10 ms here); the HW period
  setting does not apply. Buffer at least 6 mixer periods: 3 and 4 slipped live (below). A Shared
  input's FIFO counts packets as up to two mixer periods at once (the mixer hands them over on its
  own thread).
- **Auto**: Exclusive, else Shared.
- No raw KS and no MME.
- `WHAMasterStats` appends `hwOutMode` / `hwInMode` (as opened, -1 = not open) and
  `hwOutRequestedMode` / `hwInRequestedMode`.

## Consequences
- Live on the dev VM (`asio-live`, 48 kHz, block 256, no DAW open):
  - Loops on VB-Cable (CABLE Input -> CABLE Output), 8 s each, delay "steady" = no slipped frame:

    | Output (Master Clock) | Input | Steady |
    |---|---|---|
    | Exclusive | Exclusive | 5 / 5 |
    | Exclusive | **Shared** | 3 / 3, bit-exact |
    | **Shared** | Exclusive | 0 / 3 |
    | **Shared** | **Shared**, 48 kHz | 6 / 16 |
    | **Shared** | **Shared**, 44.1 kHz (= the devices' Windows format) | 2 / 5 |

    That was before the fixes below (same day). A Shared output as the Master Clock slipped: the input
    read false clock drift and resampled, or starved and its target grew by two mixer periods.
  - Fixes (2026-09-25, found with per-packet capture traces and the clock trace):
    - `HwClockPacer` learned the Windows mixer's chunk from its reports: 480 frames per wake, 960
      after a late one, so the chunk flipped 512 <-> 1024 and each flip moved the setpoint, a jump of
      ~512 frames in the Master Clock's phase against the device. `reset` now takes the mixer period
      (Shared devices): chunk two periods from the start, expected fill setpoint + one period (the
      output latency reported was ~370 frames too high), the warm-up aiming at that fill. A missed wake
      leaves the mixer a period behind for ~250 ms before a double take; the 1 Hz timeline followed it
      and hurried afterwards, so after the warm-up it runs at 0.1 Hz and is not restarted (it keeps the
      phase it locked at 1 Hz, the rate starts at nominal). Offline (`hw-clock-test`, a mixer model):
      phase against the device wanders 164 frames against 1039 before. Weighting late reports less
      looked better offline but biased the rate ~1000 ppm live; not used.
    - A Shared capture packet is stamped at its first frame; `HwInputFifo` takes the time of the
      newest frame, so the backlog counted the newest packet twice and held a mixer period less queued
      than its target (as low as 257 frames): one late packet starved it. `KsCapture` now adds the
      packet's length to a Shared stamp. Reported input latency is unchanged; the real queue is ~10 ms
      deeper (it was ~10 ms less than reported).
  - After the fixes, 8 s loops (two runs whose Master Clock thread the VM stalled for 47 and 124 ms
    are left out; it hurried to catch up as designed):

    | Output (Master Clock) | Input | Steady | Input bit-exact | Starved |
    |---|---|---|---|---|
    | **Shared** | **Shared** | 3 / 5 | 5 / 5 | 0 |
    | **Shared** | Exclusive | 6 / 9 | 7 / 9 | 0 |
    | Exclusive | **Shared** | 3 / 3 | 3 / 3 | 0 |
    | Exclusive | Exclusive | 13 / 14 | 13 / 14 | 0 |

    On the way there (not the final code), the Shared input fix alone took Shared / Shared from 18
    starves in 30 runs to 0 in 12, and the fixed chunk from 3 / 6 runs resampling to 0 / 48.

    Every remaining step with the input bit-exact is VB-Cable's own: at those moments our render
    stream was regular (the mixer took 480 or 960, the fill never below 1054) and the capture stream
    continuous (device position +441 per packet, no flags); a plain WASAPI loop on the same cable
    with no WinHookAudio driver steps the same way (Shared 1 / 8, Exclusive 3 / 10 runs, +134..+547
    frames). An exclusive input under a Shared Master Clock still resamples now and then: its drift
    dead band (~120 frames) is below the Master Clock's remaining wander against the mixer. Output #1
    Exclusive is the steadier choice for a DAW that records through Exclusive inputs. Not measured on
    physical hardware.
  - Buffer: with 3 mixer periods the output ran empty; 4 slipped in one run of three; 6 is used. A
    Shared input FIFO counting packets as two mixer periods removed a late starve (+497 frames).
  - Loop SNR is 88.6 dB when the Master Clock runs at the devices' Windows rate (44.1 kHz here, Shared
    both ways) and below 10 dB at 48 kHz in every mode: the low SNR seen on this VM since 0012 comes
    from the rate mismatch, not from the driver.
  - Round trip at 48 kHz: ~172 ms Shared both ways against ~101 ms Exclusive on the same cable; our
    part ~105 ms (out 44 + in 61) against ~59 ms.
  - VB-Audio "CABLE In 16ch" Shared: opens with 16 channels (the mixer's), loop steady.
  - HD Audio Shared as Master Clock: 0 underruns; its clock runs ~0.75 % fast (Exclusive ~0.46 %
    on the same VM device).
  - A second client can open a Shared device's endpoint while the DAW streams (checked by asio-live).
  - Auto chose Exclusive on every device here (HD Audio at 44.1 / 48 / 96 kHz, VB-Cable, Hi-Fi
    Cable, WinHookAudio Output 1): none refuses exclusive mode, so **the fallback to Shared is not
    verified live**. It needs a device with "Allow applications to take exclusive control" off.
  - An app playing through the mixer did not stop Exclusive from opening (VB-Cable): Windows gave
    the device to the exclusive stream. An app holding a device exclusively blocks both modes, so the
    "in use" error does not suggest Shared.
  - Exclusive on VB-Cable's "CABLE Input" blocks its sibling "CABLE In 16ch" even in Shared
    (`AUDCLNT_E_DEVICE_IN_USE`): one driver pin behind two endpoints.
- SNR in the loop stays low in both modes on this VM (a known issue from 0012); no ear check.
