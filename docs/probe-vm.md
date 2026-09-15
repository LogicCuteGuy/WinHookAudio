# WinHookAudio device probe for a Windows x64 VM

This package contains the native C++20 capability probe for issue #2. It is a
console application, not yet the ASIO driver or native patchbay. No driver
installation, ASIO registration, SDK download, browser, or VC runtime installation
is required. The C++ runtime is linked statically.

## Run in the VM

1. Extract the ZIP into a writable folder inside a Windows x64 VM.
2. Open PowerShell in that folder and run `./Run-Probe.ps1`.
3. Keep the timestamped JSON under `results/` and inspect its `errors` fields.

If PowerShell script execution is restricted, run the executable directly; no
policy change is needed:

```powershell
./bin/winhookaudio-probe.exe discover > discovery.json
$inventory = Get-Content -Raw ./discovery.json | ConvertFrom-Json
```

VM-emulated audio and USB devices passed through to the guest can expose different
KS capabilities. Test the devices visible inside the guest. Host-machine IDs are
not a configuration for the VM. This probe does not configure USB passthrough.

`discover` reads MMDevice properties, mix-format/exclusive-support queries and
WASAPI periods, KS filter/pin properties, and both ASIO registry views. It opens
KS **filter handles for property queries**, but never creates streaming pins,
initializes WASAPI streams, changes default devices, changes volume, or changes
routes. ASIO registration is inventory only; no ASIO DLL is loaded.

## Explicit pin-open check

Run this only in the VM when you want to test a specific pin. Pick a `ks.filters`
entry and pin from its JSON. Use the full `path` and `pin_id`, an advertised PCM
format, and the supported interface. Standard interface ID 0 is `streaming`; ID 1
is `looped`. Topology/bridge pins with `communication: 0` are not streaming pins.
Directions are from the DAW's perspective: `to_daw` is capture; `from_daw` is render.

The following is a command template; replace every placeholder with VM evidence:

```powershell
$filterPath = '<full KS filter path from this VM inventory>'
./bin/winhookaudio-probe.exe open --filter $filterPath --pin <pin-id> --rate <Hz> --channels <count> --bits <16-or-24-or-32> --valid-bits <bits> --channel-mask <decimal-mask> --interface <streaming-or-looped> --wave-format <pcm-or-extensible> > pin-open.json
```

All nine options are required. Numeric values use unsigned decimal. A channel mask
of 0 means unspecified positions; stereo left/right is 3. Legacy `pcm` requires
at most two channels, equal valid/container bits, and mask 0. Probe bounds are
8,000–768,000 Hz and 1–64 channels; these are tool limits, not product capacity or
hardware support promises. Only integer PCM is currently supported for opening.

`open` attempts one pin creation and closes its handle before returning. It does
not transition to ACQUIRE/PAUSE/RUN, allocate an audio buffer, capture, or play
audio. A successful open would verify acceptance of that connection request only;
it does not prove simultaneous duplex operation, usable periods, synchronized
clocks, latency, or streaming. KS periods remain explicitly unprobed. Do not infer
KS support from the WASAPI mix format or period.

Exit codes: 0 = report emitted / selected pin opened; 1 = fatal failure;
2 = invalid command; 3 = selected pin could not be opened. Discovery can exit 0
with partial results; read the per-device and per-property errors. JSON uses schema
version 1 and UTF-8. Unknown/missing data remains null, absent, or explicitly
unprobed. `stream_verified` is always false in this version.

`api_view_matches` links MMDevice adapter and KS views by the same PnP instance.
These are device groups, not proof of an exact pin mapping or shared clock. Multiple
endpoints can share the same group. Resolve the mapping before using multiple APIs
on the same device. An unmatched identity does not establish independent hardware.

Device-property calls execute synchronously. A stalled third-party driver can stall
the probe; there is no per-driver timeout/isolation worker in this version.

## Current verification status

Before testing was moved to the VM, discovery produced valid inventory on the
development PC. Selected BOMGE USB capture/render open requests returned Windows
error 87 (`ERROR_INVALID_PARAMETER`). No successful pin open or streaming has been
verified. The final package has build and offline CLI checks only; its hardware
acceptance is pending your VM results. Issue #2 remains incomplete until a usable
first path is established.

For a VM report, include the JSON, Windows version, hypervisor and audio/USB setup,
installed DAW/version, chosen capture/render device, and exact open commands. Use
one duplex device where possible; separate devices require clock adaptation unless
a shared clock is verified. No DAW compatibility has been tested yet.

## Build from source

Install CMake 3.24+ and MSVC with the Windows SDK. From the repository root:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -C Release -B build/packages
```

CTest only runs help and malformed-command checks; it never enumerates or opens
audio devices. The ZIP contains the executable, this guide, and the VM launcher.
The HTML prototype is excluded. This is a development diagnostic package; release
licensing and code signing remain separate project decisions.

## API references

- [KS pin properties](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/ne-ks-ksproperty_pin)
- [KS pin creation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/nf-ks-kscreatepin)
- [Windows audio format descriptors](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/extensible-wave-format-descriptors)
