#pragma once

// WinHookAudio WHAA packet — offline header-only.
// Vocabulary: Network Stream.
// No sockets, no encode/decode, no threads — pure header and offline validation.
// WHAA My Version Only: PCM_F32 / PCM_I16 / VORBIS, magic v5, per-stream codec.

#include <cstdint>
#include <cstring>
#include <string>

#include "WHASlotTable.h"

#define WHAA_MAGIC 0x41414857u  // 'WHAA' little-endian
#define WHAA_VERSION 5u
#define WHAA_MAX_PAYLOAD 65507u  // UDP safe
#define WHAA_CODEBOOK_REDUNDANT 3u

namespace wha {

enum WHAAPacketType : uint32_t { WHAA_AUDIO = 0, WHAA_CODEBOOK = 1, WHAA_HELLO = 2 };

#pragma pack(push, 1)
struct WHAAPacketHeader {
  uint32_t magic = WHAA_MAGIC;
  uint32_t version = WHAA_VERSION;
  uint32_t packetType = WHAA_AUDIO;
  uint32_t codec = WHA_PCM_F32;
  uint32_t channels = 0;
  uint32_t frames = 0;
  uint32_t streamId = 0;
  uint32_t sequence = 0;
  uint64_t qpc = 0;
  uint32_t payloadBytes = 0;
};

struct WHAACodebookHeader {
  uint32_t magic = WHAA_MAGIC;
  uint32_t version = WHAA_VERSION;
  uint32_t packetType = WHAA_CODEBOOK;
  uint32_t streamId = 0;
  uint32_t sampleRate = 48000;
  uint32_t channels = 0;
  float quality = 0.4f;
  uint32_t headersBytes = 0;
};
#pragma pack(pop)

// ---- Offline validation (no sockets) ----

constexpr bool IsValidWhaaMagic(uint32_t magic) { return magic == WHAA_MAGIC; }
constexpr bool IsValidWhaaVersion(uint32_t version) { return version == WHAA_VERSION; }

constexpr bool IsValidWhaaCodec(uint32_t codec) {
  return codec == WHA_PCM_F32 || codec == WHA_PCM_I16 || codec == WHA_VORBIS;
}

constexpr bool IsValidWhaaChannels(uint32_t codec, uint32_t channels) {
  if (channels < 1) return false;
  if (codec == WHA_VORBIS) return channels <= 128;
  return channels <= 32;
}

constexpr bool IsValidWhaaStreamId(uint32_t id) { return id < WHA_NET_STREAMS; }

constexpr bool IsValidWhaaPayload(uint32_t bytes) { return bytes <= WHAA_MAX_PAYLOAD; }

inline bool ValidateWhaaHeader(const WHAAPacketHeader& h, std::string* error) {
  auto fail = [&](const char* msg) -> bool {
    if (error) *error = msg;
    return false;
  };
  if (!IsValidWhaaMagic(h.magic)) return fail("magic invalid");
  if (!IsValidWhaaVersion(h.version)) return fail("version invalid");
  if (h.packetType != WHAA_AUDIO) return fail("packetType not audio");
  if (!IsValidWhaaCodec(h.codec)) return fail("codec invalid");
  if (!IsValidWhaaChannels(h.codec, h.channels)) return fail("channels invalid");
  if (h.frames == 0 || h.frames > 4096) return fail("frames invalid");
  if (!IsValidWhaaStreamId(h.streamId)) return fail("streamId invalid");
  if (!IsValidWhaaPayload(h.payloadBytes)) return fail("payloadBytes too large");
  // PCM payload size check: channels * frames * bytesPerSample
  if (h.codec == WHA_PCM_F32) {
    uint32_t expected = h.channels * h.frames * 4;
    if (h.payloadBytes != expected) return fail("pcm_f32 payload mismatch");
  } else if (h.codec == WHA_PCM_I16) {
    uint32_t expected = h.channels * h.frames * 2;
    if (h.payloadBytes != expected) return fail("pcm_i16 payload mismatch");
  }
  // Vorbis payload is variable compressed, just bounds check
  return true;
}

inline bool ValidateWhaaCodebook(const WHAACodebookHeader& h, std::string* error) {
  auto fail = [&](const char* msg) -> bool {
    if (error) *error = msg;
    return false;
  };
  if (!IsValidWhaaMagic(h.magic)) return fail("magic invalid");
  if (!IsValidWhaaVersion(h.version)) return fail("version invalid");
  if (h.packetType != WHAA_CODEBOOK) return fail("packetType not codebook");
  if (!IsValidWhaaStreamId(h.streamId)) return fail("streamId invalid");
  if (h.sampleRate != 44100 && h.sampleRate != 48000 && h.sampleRate != 96000)
    return fail("sampleRate invalid");
  if (h.channels < 1 || h.channels > 128) return fail("channels invalid");
  if (h.quality < 0.1f || h.quality > 1.0f) return fail("quality invalid");
  if (h.headersBytes == 0 || h.headersBytes > WHAA_MAX_PAYLOAD) return fail("headersBytes invalid");
  return true;
}

static_assert(sizeof(WHAAPacketHeader) == 44, "WHAA header packed size");
static_assert(sizeof(WHAACodebookHeader) == 32, "WHAA codebook header packed size");

}  // namespace wha
