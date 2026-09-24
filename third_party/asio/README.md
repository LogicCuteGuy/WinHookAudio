# ASIO SDK 2.3.4

Steinberg ASIO SDK 2.3.4 is used by `asio-master/` and `asio-bridge/` (the IASIO the DAW calls).

## License

Since 2.3.4 the SDK is dual-licensed: the proprietary Steinberg ASIO License **or** GPLv3
(`LICENSE.txt`). Building locally works under either. **Before distributing the driver, choose one:**
GPLv3 makes the whole driver GPLv3; the proprietary license needs a signed Steinberg agreement
(www.steinberg.net/en/company/developers.html). ADR 0007 assumed the proprietary license.

## Fetch

Official: https://www.steinberg.net/asiosdk. The copy vendored on 2026-09-23 came from
https://github.com/audiosdk/asio (an unofficial mirror of 2.3.4 — `changes.txt` "new DUAL licensing").
Only the source/text files were taken (no PDFs, artwork or `asio.opt`) and scanned before use.
Keep the SDK layout:

```
third_party/asio/common/asio.h
third_party/asio/common/asiosys.h
third_party/asio/common/iasiodrv.h
third_party/asio/host/asiodrivers.cpp
third_party/asio/host/sample/hostsample.cpp
```

CMake detects `common/asio.h` + `common/iasiodrv.h`, defines `WHA_HAVE_ASIO_SDK=1` and puts
`common/` on the include path (target `wha_asio`). Override with `-DWHA_ASIO_DIR=`.

## Offline build

Without the SDK, `common/WHAAsio.h` provides an exact mirror of the same declarations (global
namespace, same types/values, `#pragma pack(4)`, size `static_assert`s). `asio-dll-host-offline`
loads the built DLLs through their exports with whichever headers are active, so both builds check
the same vtable.

This directory is `.gitignore`d except for this README.
