#pragma once

// WHANetwork — UDP 6980 sendto/recvfrom + CODEBOOK for WHAA PCM.
// Vocabulary: Network Stream, Worker.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "WHAPacket.h"
#include "WHASlotTable.h"
#include "WHAJitterBuffer.h"

namespace wha {

// CODEBOOK: 3 Vorbis headers (ident/comment/codebook), sent 3x as WHAA_CODEBOOK.
// headers layout: u32 len0, u32 len1, u32 len2, then the 3 header packets back to back.
struct WHACodebook {
  uint32_t streamId = 0;
  uint32_t sampleRate = 48000;
  uint32_t channels = 2;
  float quality = 0.4f;
  std::vector<uint8_t> headers;
};

// 15: true only when built against vendored libvorbis/libogg (WHA_HAVE_VORBIS) or r8brain (WHA_HAVE_R8BRAIN).
bool VorbisAvailable();
bool ResamplerAvailable();

// Vorbis Tx — one per Network Stream. Emits raw Vorbis packets (no Ogg paging; WHAA header frames them).
// Open() fails when libvorbis is not vendored; callers fall back to PCM.
class WHAVorbisEncoder {
 public:
  WHAVorbisEncoder();
  ~WHAVorbisEncoder();
  WHAVorbisEncoder(const WHAVorbisEncoder&) = delete;
  WHAVorbisEncoder& operator=(const WHAVorbisEncoder&) = delete;

  bool Open(uint32_t streamId, uint32_t sampleRate, uint32_t channels, float quality, WHACodebook& codebook);
  // Interleaved float32 in; appends zero or more packets (Vorbis buffers internally).
  bool Encode(const float* interleaved, uint32_t frames, std::vector<std::vector<uint8_t>>& packets);
  // End of stream: drains the remaining packets.
  bool Flush(std::vector<std::vector<uint8_t>>& packets);
  void Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Vorbis Rx — one per Network Stream, primed by a received CODEBOOK.
class WHAVorbisDecoder {
 public:
  WHAVorbisDecoder();
  ~WHAVorbisDecoder();
  WHAVorbisDecoder(const WHAVorbisDecoder&) = delete;
  WHAVorbisDecoder& operator=(const WHAVorbisDecoder&) = delete;

  bool Open(const WHACodebook& codebook);
  // Appends decoded interleaved float32 frames (may append none while the decoder primes).
  bool Decode(const uint8_t* packet, uint32_t bytes, std::vector<float>& interleavedOut);
  void Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// r8brain SRC when remote SR differs — keeps filter state across blocks.
class WHAResampler {
 public:
  WHAResampler();
  ~WHAResampler();
  WHAResampler(const WHAResampler&) = delete;
  WHAResampler& operator=(const WHAResampler&) = delete;

  bool Open(uint32_t inRate, uint32_t outRate, uint32_t channels, uint32_t maxInFrames);
  // Interleaved float32 in; appends resampled interleaved frames (none until the filter fills).
  bool Process(const float* interleaved, uint32_t frames, std::vector<float>& interleavedOut);
  void Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Build WHAA PCM payload from SHM Out (float32)
std::vector<uint8_t> BuildPcmPayload(const float* data, uint32_t channels, uint32_t frames, WHACodec codec);
// Parse WHAA PCM payload to SHM In (float32)
bool ParsePcmPayload(const uint8_t* payload, uint32_t payloadBytes, float* out, uint32_t channels, uint32_t frames, WHACodec codec);

// CODEBOOK: checks fields and that headers holds exactly 3 length-prefixed packets
bool ValidateCodebook(const WHACodebook& cb);

// UDP helpers (offline: loopback 127.0.0.1:6980 without firewall)
bool SendWhaaPacket(const WHAAPacketHeader& header, const uint8_t* payload, const char* ip, uint16_t port);
bool RecvWhaaPacket(WHAAPacketHeader& header, std::vector<uint8_t>& payload, std::string& fromIp, uint16_t& fromPort);

}  // namespace wha
