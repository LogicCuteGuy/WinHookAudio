# Third-party notices

WinHookAudio's own code is under the [MIT License](LICENSE). The release binaries also contain
the libraries below. Their source is not stored in this repository: `scripts/Fetch-ThirdParty.ps1`
downloads the pinned versions into `third_party/` (ignored by git).

| Library | Version | Used in | License |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.90 | Control Panel (both ASIO DLLs) | MIT, Copyright (c) 2014-2023 Omar Cornut |
| [libogg](https://github.com/xiph/ogg) | 1.3.5 | WHAA network stream (Master DLL) | BSD-3-Clause, Copyright (c) 2002 Xiph.org Foundation |
| [libvorbis](https://github.com/xiph/vorbis) | 1.3.7 | WHAA network stream (Master DLL) | BSD-3-Clause, Copyright (c) 2002-2020 Xiph.org Foundation |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | 7.5 | WHAA sample-rate conversion (Master DLL) | MIT, Copyright (c) 2013-2026 Aleksey Vaneev |

The full license texts are in each library's source tree (`LICENSE.txt` / `COPYING` / `LICENSE`).

## ASIO

ASIO is a trademark and software of Steinberg Media Technologies GmbH.

Release builds do **not** use the Steinberg ASIO SDK. They compile against
`common/WHAAsio.h`, this project's own declaration of the ASIO driver interface that DAWs call.
A developer can put the SDK in `third_party/asio/` for local builds (see
`third_party/asio/README.md`). The SDK is dual-licensed (Steinberg ASIO License or GPLv3), so
binaries built with it must not be released under the MIT License alone.

## Windows Driver Kit

`WinHookAudio.sys` links the Windows Driver Kit's kernel libraries (`ntoskrnl.lib`, `portcls.lib`,
`ks.lib`, `libcntpr.lib`, ...), as every Windows driver does, under the WDK license terms.
