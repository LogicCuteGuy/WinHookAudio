# libvorbis 1.3.7 + libogg 1.3.5

libvorbis 1.3.7 + libogg 1.3.5 are required for `WHAA` `VORBIS Q0.1-1.0` `64-500 kbps` per stereo.

## Fetch

Download `libvorbis-1.3.7.tar.gz` and `libogg-1.3.5.tar.gz` from https://downloads.xiph.org/releases/ and extract the full source trees so that:

```
third_party/libvorbis/include/vorbis/codec.h
third_party/libvorbis/include/vorbis/vorbisenc.h
third_party/libvorbis/lib/*.c
third_party/libogg/include/ogg/ogg.h
third_party/libogg/src/framing.c
```

CMake detects both trees, builds static `wha_ogg` + `wha_vorbis`, and defines `WHA_HAVE_VORBIS=1` (`common/network/WHACodec.cpp`). Override the locations with `-DWHA_OGG_DIR=` / `-DWHA_VORBIS_DIR=`.

## Offline build

`wha_common` + `abi-check` + `probe` + `host-sample` + `worker-test` + `ks-test` + `panel-test` + `whaa-test` + `virtual-test` + `installer-test` build without libvorbis via stub (`stream_verified:false`): `WHAVorbisEncoder/Decoder::Open()` return false and callers keep PCM.

This directory is `.gitignore`d except for this README.
