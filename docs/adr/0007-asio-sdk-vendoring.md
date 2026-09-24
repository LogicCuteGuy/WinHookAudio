# ASIO SDK 2.3.3 vendoring under third_party/asio

Date: 2026-09-15
Status: accepted (release builds: see 0013, they use common/WHAAsio.h, not the SDK)

## Context
Master Driver streaming needs `IASIO` (`DllGetClassObject`, `getChannels`, `getChannelInfo`, `bufferSwitch`, `controlPanel`, `hostCallback(ASIOResetRequest)`). `docs/implementation-plan.md:9` says pin SDK version and licensing before incorporating. Repo has no `asio.h`/`iasiodrv.h` — only `src/probe/asio_registry.cpp` registry enumeration. Datasheet FINAL v10 lists `ASIO SDK 2.3.3` as dependency. `DESKTOP-R1VU4L4` has Bitwig as Master host but tracer also uses SDK `host/sample` for offline gate without DAW.

## Decision
Vendor `ASIO SDK 2.3.3` under `third_party/asio/` (Steinberg license). `.gitignore` the SDK payload, document fetch step in `README.md`/`docs/architecture.md`. For offline build (`ctest` without admin/DAW), stub `IASIO` interface locally so `wha_common` + `abi-check` stay green with `stream_verified:false`. Real SDK fetched only for runtime verification (SDK host sample then Bitwig in elevated new `cmd`).

## Consequences
- Offline gate stays green without licensing friction or admin.
- Runtime gate (`08`→`10`) requires local SDK fetch + elevated terminal for driver registration.
- `third_party/asio/` is not redistributed publicly; CI must fetch via documented step.
- ADR 0002 (Pure DLL Worker) unchanged — Worker remains inside Master DLL, not engine process.
