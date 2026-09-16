#pragma once

// WHANetwork — UDP 6980 sendto/recvfrom + CODEBOOK for WHAA PCM.
// Vocabulary: Network Stream, Worker.

#include <cstdint>
#include <string>
#include <vector>
#include "WHAPacket.h"
#include "WHASlotTable.h"
#include "WHAJitterBuffer.h"

namespace wha {

// CODEBOOK: 3 Vorbis headers (ident/comment/codebook) — stub as PCM fallback for 14
struct WHACodebook {
  uint32_t streamId = 0;
  uint32_t sampleRate = 48000;
  uint32_t channels = 2;
  float quality = 0.4f;
  std::vector<uint8_t> headers;  // concatenated 3 headers
};

// Build WHAA PCM payload from SHM Out (float32)
std::vector<uint8_t> BuildPcmPayload(const float* data, uint32_t channels, uint32_t frames, WHACodec codec);
// Parse WHAA PCM payload to SHM In (float32)
bool ParsePcmPayload(const uint8_t* payload, uint32_t payloadBytes, float* out, uint32_t channels, uint32_t frames, WHACodec codec);

// CODEBOOK: build 3 headers (stub)
WHACodebook BuildCodebook(uint32_t streamId, uint32_t sampleRate, uint32_t channels, float quality);
bool ValidateCodebook(const WHACodebook& cb);

// UDP helpers (offline: loopback 127.0.0.1:6980 without firewall)
bool SendWhaaPacket(const WHAAPacketHeader& header, const uint8_t* payload, const char* ip, uint16_t port);
bool RecvWhaaPacket(WHAAPacketHeader& header, std::vector<uint8_t>& payload, std::string& fromIp, uint16_t& fromPort);

}  // namespace wha
