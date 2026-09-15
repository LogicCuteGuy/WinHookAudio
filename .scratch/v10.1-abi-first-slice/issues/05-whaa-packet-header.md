# 05: Network packet header

**What to build:** An offline Network Stream packet definition for WHAA-only transport with PCM and Vorbis codecs plus codebook handshake.

**Blocked by:** 01-slot-table-abi.

**Status:** ready-for-agent

- [ ] Header carries magic version, codec, channels, frames, stream identifier, sequence, clock stamp, and payload length
- [ ] Vorbis handshake carries codebook headers with redundant send on Save
- [ ] Invalid codec, channel count, stream identifier, and oversized payload are rejected
- [ ] No sockets opened; offline checks only with no network use
