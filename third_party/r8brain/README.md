# r8brain-free 7.5

r8brain-free is required for `WHAA` `Vorbis` `SRC` if remote `SR` differs.

## Fetch

Download from https://github.com/avaneev/r8brain-free-src (7.5, header-only) and extract so that:

```
third_party/r8brain/r8bbase.h
third_party/r8brain/CDSPResampler.h
```

CMake detects the tree and defines `WHA_HAVE_R8BRAIN=1` for `WHAResampler` (`common/network/WHACodec.cpp`). Override with `-DWHA_R8B_DIR=`.

## Offline build

`wha_common` + `abi-check` + `probe` + `host-sample` + `worker-test` + `ks-test` + `panel-test` + `whaa-test` + `virtual-test` + `installer-test` build without r8brain via stub (`stream_verified:false`).

This directory is `.gitignore`d except for this README.
