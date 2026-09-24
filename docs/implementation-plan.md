# Implementation stages

Status: proposed; only the design, UI concept, and registry inventory exist. No stage below is claimed complete. Each stage must yield audible/measurable evidence before expanding.

Confirmed implementation contract: C/C++ project; the DAW hosts the project's ASIO driver DLL. Use C++20 for user-mode code and the native Win32 control panel, with WDK-compatible C/C++ for the separate cable kernel driver. Build user-mode targets with CMake/MSVC and the cable package using the WDK toolchain. The existing HTML concept is a design reference, not production GUI code.

## 1. Probe and define the first real path

The product targets any compatible Windows ASIO DAW. Use one available host and one physical output for the first measured path, then validate a matrix of independent hosts before claiming broad compatibility. Enumerate MMDevice and KS filter/pin capabilities, stable IDs, ASIO registrations, supported formats, and periods. Detect multiple API views of the same physical device and avoid opening both. Inventory registration separately from open/stream success. Pin ASIO SDK version and licensing choice before incorporating it.

Acceptance: machine-readable capabilities, explicit open-failure reasons, no routing changes during enumeration; select a format the actual device accepts. Registry discovery supplied here is only preliminary evidence.

## 2. Primary ASIO driver and one endpoint

Implement the C++ x64 ASIO DLL that the DAW loads, broker transport, one output adapter, and fixed 32-in/32-out bank. A DAW-generated test signal reaches hardware. Add one capture endpoint and demonstrate capture → DAW monitor/effect → output. WASAPI can validate the first transport quickly; separately implement and test native KS before claiming KS support. Implement a minimal native C++ panel reachable through the driver's control-panel entry. A helper process supplies transport/isolation only; the DAW remains the primary ASIO host.

Acceptance: driver appears in the chosen DAW; sample-position and callback cadence remain consistent; named channel order stays stable; hardware loopback measures latency; output is silent after DAW crash, broker crash, or disconnect. Test start/stop/reopen and unsupported formats. Record actual configuration and results.

## 3. Clock adaptation and device aggregation

Add another independent clock, adaptive resampling, endpoint FIFOs, format conversion, and block adaptation. Add native KS capture/render capability paths and a hardware ASIO worker where needed. Commit validated mappings at block boundaries. Implement input-overlap rejection and output fan-out.

Acceptance: one-hour two-device soak, no steadily growing/shrinking buffer occupancy; simulated ±100 ppm clocks stay bounded; unplugging a non-master leaves other routes running; master loss stops/mutes predictably. Publish measured latency and underrun statistics for low, balanced, and high profiles on the target hardware. Small buffer selection alone does not pass this stage.

## 4. App cable and ASIO bridge

First validate app → existing cable → DAW → separate return cable → app. Then implement the owned WDM/WaveRT playback/recording endpoints and real broker transfer. SYSVAD is sample infrastructure, not an already working cable. Add an independent bridge-driver client slot and client ownership/lifecycle.

Acceptance: a Windows app and a secondary ASIO app each send distinct tones into separately named DAW inputs, and each receives the selected processed return. No shared-ring contention or automatic return feedback. Test mismatched client blocks/rates, restarting one client, and device-busy errors. Kernel driver deployment/signing is a separate actual prerequisite, not satisfied by the ASIO DLL.

## 5. LAN transport

Confirm the peer/protocol. For proposed VBAN, implement/validate PCM framing and sequence handling against the published specification, separate network workers, bounded jitter buffers, drift adaptation, and per-stream settings.

Acceptance: bidirectional audio against the chosen real peer; distinguish packet delay, loss, reordering, duplication, and clock drift in deterministic fixtures; reject malformed/oversized payloads; re-prime after format changes; measure latency and loss on wired LAN. Local DAW monitoring remains stable while the LAN path is disturbed.

## 6. Production control panel and packaging

Implement the native C++/Win32 two-row grid with arbitrary channel maps and horizontally scalable columns, actual capability choices, diagnostics, preset persistence, transactional apply, and stable unavailable-device slots. GUI settings show requested versus actual state. Route changes must not falsely appear applied after engine rejection. Add explicit recovery workflows and deployment packages.

Acceptance: DAW opens the panel; closing the panel preserves audio; saving/reloading reproduces all channel assignments; unsupported choices are rejected before activation; device reorder/unplug does not reroute channels; stop/apply/restart updates the DAW correctly. Complete driver package/signing tests and selected DAW compatibility tests before calling the environment usable.

Deployment packages (ADR 0013): `installer/WinHookAudio.iss` + `winhookaudio-devsetup` build in CI with the test-signed driver; the Microsoft-signed driver (attestation, EV certificate) and SignPath signing of the user-mode files are still to do (`docs/signing.md`).

## Critical failure cases to preserve

- “All devices” means all discovered compatible/available endpoints, not guaranteed simultaneous use of every driver or occupied pin.
- A two-row summary is not an N×N crosspoint matrix. The DAW supplies the full routing graph.
- The engine cannot inspect arbitrary DAW/plugin/acoustic feedback paths; never claim complete automatic feedback prevention.
- One-way cable send/return pairing must not create an implicit loop.
- Nominally equal sample rates still require clock handling unless synchronization is verified.
- GUI-only simulation cannot validate audio, device formats, ASIO behavior, or network performance.
