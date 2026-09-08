# WinHookAudio

A Windows ASIO driver project written in **C/C++**, loaded and hosted by the user's DAW.

`Sources → WinHookAudio ASIO inputs → DAW processing/routing → WinHookAudio ASIO outputs → destinations`

The DAW is the ASIO host and owns mixing, effects, buses, and monitoring. This project provides the ASIO driver DLL, device access, channel assignment, buffering, clock adaptation, and a native C++ two-row configuration panel opened from the DAW's ASIO settings. Sources and destinations include KS devices, virtual cables, secondary ASIO clients, and LAN streams.

Production language: C++20 for the user-mode driver, audio runtime, adapters, and native GUI; C-compatible interfaces where needed, and WDK-compatible C/C++ for the separate virtual cable kernel driver. The earlier HTML prototype is a layout reference only.

**Current state: research, architecture, and an interactive UI concept. No audio engine, ASIO driver, virtual cable driver, or LAN transport has been implemented or installed.** The prototype uses illustrative devices and never accesses audio hardware.

- [Architecture and requirements](docs/architecture.md)
- [Implementation stages and acceptance checks](docs/implementation-plan.md)
- [Windows audio research](docs/research/windows-audio-constraints.md)
- [Interactive patchbay concept](prototype/index.html) — open in a browser; no dependencies.
- [Read-only inventory script](scripts/Get-AudioInventory.ps1)

Confirmed scope: expose devices and virtual cables to the DAW, with a universal Windows ASIO-host target rather than a dependency on one DAW. Intercepting streams already owned by other applications is not required. Actual DAW compatibility needs testing; the primary hardware interface, first LAN peer, and distribution model remain unconfirmed.

The design defaults below are proposals, not measured device capabilities: 32 DAW inputs + 32 outputs, 48 kHz, a 128-frame processing block, and float32 transport. Hardware formats and supported periods must be probed before implementation accepts a configuration.
