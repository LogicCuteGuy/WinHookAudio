# libvorbis 1.3.7 + libogg 1.3.5

libvorbis 1.3.7 + libogg 1.3.5 are required for `WHAA` `VORBIS Q0.1-1.0` `64-500 kbps` per stereo.

## Fetch

Download from https://xiph.org/downloads/ and extract so that:

```
third_party/libvorbis/vorbis/codec.h
third_party/libvorbis/vorbis/vorbisenc.h
third_party/libogg/ogg/ogg.h
```

## Offline build

`wha_common` + `abi-check` + `probe` + `host-sample` + `worker-test` + `ks-test` + `panel-test` + `whaa-test` + `virtual-test` + `installer-test` build without libvorbis via stub (`stream_verified:false`, Vorbis as PCM fallback).

This directory is `.gitignore`d except for this README.
