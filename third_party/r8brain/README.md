# r8brain-free

r8brain-free is required for `WHAA` `Vorbis` `SRC` if remote `SR` differs.

## Fetch

Download from https://github.com/avaneev/r8brain-free and extract so that:

```
third_party/r8brain/r8bbase.h
third_party/r8brain/CDSPResampler.h
```

## Offline build

`wha_common` + `abi-check` + `probe` + `host-sample` + `worker-test` + `ks-test` + `panel-test` + `whaa-test` + `virtual-test` + `installer-test` build without r8brain via stub (`stream_verified:false`).

This directory is `.gitignore`d except for this README.
