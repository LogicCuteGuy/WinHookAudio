#include "WHANetwork.h"
#include <cstring>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace wha {

std::vector<uint8_t> BuildPcmPayload(const float* data, uint32_t channels, uint32_t frames, WHACodec codec) {
  std::vector<uint8_t> payload;
  if (codec == WHA_PCM_F32) {
    payload.resize(channels * frames * 4);
    std::memcpy(payload.data(), data, payload.size());
  } else if (codec == WHA_PCM_I16) {
    payload.resize(channels * frames * 2);
    int16_t* out = reinterpret_cast<int16_t*>(payload.data());
    for (uint32_t i = 0; i < channels * frames; ++i) {
      float v = data[i];
      if (v > 1.0f) v = 1.0f;
      if (v < -1.0f) v = -1.0f;
      out[i] = static_cast<int16_t>(v * 32767.0f);
    }
  }
  return payload;
}

bool ParsePcmPayload(const uint8_t* payload, uint32_t payloadBytes, float* out, uint32_t channels, uint32_t frames, WHACodec codec) {
  if (codec == WHA_PCM_F32) {
    if (payloadBytes != channels * frames * 4) return false;
    std::memcpy(out, payload, payloadBytes);
    return true;
  } else if (codec == WHA_PCM_I16) {
    if (payloadBytes != channels * frames * 2) return false;
    const int16_t* in = reinterpret_cast<const int16_t*>(payload);
    for (uint32_t i = 0; i < channels * frames; ++i) out[i] = in[i] / 32767.0f;
    return true;
  }
  return false;
}

std::vector<uint8_t> VorbisEncode(const float* data, uint32_t channels, uint32_t frames, float quality) {
  // Stub: if libvorbis not available, fallback to PCM_F32
  (void)quality;
  return BuildPcmPayload(data, channels, frames, WHA_PCM_F32);
}
bool VorbisDecode(const uint8_t* payload, uint32_t payloadBytes, float* out, uint32_t channels, uint32_t frames, float quality) {
  (void)quality;
  return ParsePcmPayload(payload, payloadBytes, out, channels, frames, WHA_PCM_F32);
}
bool Resample(const float* in, uint32_t inFrames, uint32_t inRate, float* out, uint32_t outFrames, uint32_t outRate, uint32_t channels) {
  // Stub: if r8brain not available, simple linear resample or memcpy if same rate
  if (inRate == outRate) {
    uint32_t frames = inFrames < outFrames ? inFrames : outFrames;
    std::memcpy(out, in, frames * channels * sizeof(float));
    return true;
  }
  // Simple linear interpolation stub
  for (uint32_t ch = 0; ch < channels; ++ch) {
    for (uint32_t f = 0; f < outFrames; ++f) {
      double srcPos = (double)f * inFrames / outFrames;
      uint32_t srcIdx = static_cast<uint32_t>(srcPos);
      double frac = srcPos - srcIdx;
      if (srcIdx + 1 < inFrames) out[f * channels + ch] = static_cast<float>(in[srcIdx * channels + ch] * (1 - frac) + in[(srcIdx + 1) * channels + ch] * frac);
      else out[f * channels + ch] = in[srcIdx * channels + ch];
    }
  }
  return true;
}

WHACodebook BuildCodebook(uint32_t streamId, uint32_t sampleRate, uint32_t channels, float quality) {
  WHACodebook cb;
  cb.streamId = streamId;
  cb.sampleRate = sampleRate;
  cb.channels = channels;
  cb.quality = quality;
  // Stub: 3 headers as dummy bytes (ident/comment/codebook)
  cb.headers.resize(100);
  for (size_t i = 0; i < cb.headers.size(); ++i) cb.headers[i] = static_cast<uint8_t>(i & 0xFF);
  return cb;
}

bool ValidateCodebook(const WHACodebook& cb) {
  if (cb.streamId >= kNetStreams) return false;
  if (cb.sampleRate != 44100 && cb.sampleRate != 48000 && cb.sampleRate != 96000) return false;
  if (cb.channels < 1 || cb.channels > 128) return false;
  if (cb.quality < 0.1f || cb.quality > 1.0f) return false;
  if (cb.headers.empty()) return false;
  return true;
}

bool SendWhaaPacket(const WHAAPacketHeader& header, const uint8_t* payload, const char* ip, uint16_t port) {
  if (!IsValidWhaaPayload(header.payloadBytes)) return false;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) { WSACleanup(); return false; }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip, &addr.sin_addr);
  std::vector<uint8_t> packet(sizeof(header) + header.payloadBytes);
  std::memcpy(packet.data(), &header, sizeof(header));
  if (payload && header.payloadBytes) std::memcpy(packet.data() + sizeof(header), payload, header.payloadBytes);
  int sent = sendto(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0, (sockaddr*)&addr, sizeof(addr));
  closesocket(sock);
  WSACleanup();
  return sent == static_cast<int>(packet.size());
}

bool RecvWhaaPacket(WHAAPacketHeader& header, std::vector<uint8_t>& payload, std::string& fromIp, uint16_t& fromPort) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) { WSACleanup(); return false; }
  // Non-blocking with timeout 10ms
  DWORD timeout = 10;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(6980);
  addr.sin_addr.s_addr = INADDR_ANY;
  bind(sock, (sockaddr*)&addr, sizeof(addr));
  uint8_t buf[65536];
  sockaddr_in from{};
  int fromLen = sizeof(from);
  int recvd = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, (sockaddr*)&from, &fromLen);
  closesocket(sock);
  WSACleanup();
  if (recvd < static_cast<int>(sizeof(header))) return false;
  std::memcpy(&header, buf, sizeof(header));
  if (!IsValidWhaaMagic(header.magic) || !IsValidWhaaVersion(header.version)) return false;
  payload.assign(buf + sizeof(header), buf + recvd);
  char ipStr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
  fromIp = ipStr;
  fromPort = ntohs(from.sin_port);
  return true;
}

}  // namespace wha
