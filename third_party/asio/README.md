# ASIO SDK 2.3.3

Steinberg ASIO SDK 2.3.3 is required for `asio-master/` and `asio-bridge/` DLLs.

## Fetch

Download from https://www.steinberg.net/asiosdk (Steinberg license) and extract so that:

```
third_party/asio/asio.h
third_party/asio/asiodrv.h
third_party/asio/asiodrivers.cpp
third_party/asio/host/sample/
```

## Offline build

`wha_common` + `abi-check` + `probe` build without the SDK via stub `common/ASIOStub.h` (`stream_verified:false`).

## Runtime

`08→10` Master Driver slices require the real SDK for `IASIO` (`DllGetClassObject`, `getChannels`, `getChannelInfo`, `bufferSwitch`, `controlPanel`, `hostCallback(ASIOResetRequest)`) and SDK `host/sample` offline gate, then Bitwig in elevated new `cmd`.

This directory is `.gitignore`d except for this README.
