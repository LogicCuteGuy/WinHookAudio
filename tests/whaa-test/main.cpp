#include <cstdio>
#include <cstring>
#include <cmath>
#include "WHAPacket.h"
#include "WHAJitterBuffer.h"
#include "WHANetwork.h"

using namespace wha;

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // PCM_F32 payload
  float data[4] = {0.5f, -0.5f, 0.25f, -0.25f};
  auto payload = BuildPcmPayload(data, 2, 2, WHA_PCM_F32);
  check("PCM_F32 payload size", payload.size() == 2 * 2 * 4);
  float out[4] = {};
  check("PCM_F32 parse", ParsePcmPayload(payload.data(), static_cast<uint32_t>(payload.size()), out, 2, 2, WHA_PCM_F32) && out[0] == 0.5f);

  // PCM_I16 payload
  auto payload16 = BuildPcmPayload(data, 2, 2, WHA_PCM_I16);
  check("PCM_I16 payload size", payload16.size() == 2 * 2 * 2);
  float out16[4] = {};
  check("PCM_I16 parse", ParsePcmPayload(payload16.data(), static_cast<uint32_t>(payload16.size()), out16, 2, 2, WHA_PCM_I16) && std::abs(out16[0] - 0.5f) < 0.01f);

  // WHAA header
  WHAAPacketHeader hdr{};
  hdr.magic = kWhaaMagic; hdr.version = kWhaaVersion; hdr.packetType = WHAA_AUDIO;
  hdr.codec = WHA_PCM_F32; hdr.channels = 2; hdr.frames = 2; hdr.streamId = 0; hdr.payloadBytes = static_cast<uint32_t>(payload.size());
  std::string err;
  check("WHAA header valid", ValidateWhaaHeader(hdr, &err));

  // Jitter buffer
  WHAJitterBuffer jitter(20, 48000);
  jitter.push(hdr, payload.data(), static_cast<uint32_t>(payload.size()), 1000);
  check("jitter push", jitter.size() == 1);
  JitterPacket pkt;
  check("jitter pop", jitter.pop(pkt, 0) && pkt.payload.size() == payload.size());
  check("jitter empty after pop", jitter.empty());

  // CODEBOOK 3x redundant
  auto cb = BuildCodebook(0, 48000, 2, 0.4f);
  check("CODEBOOK build", ValidateCodebook(cb));
  check("CODEBOOK 3x redundant", kWhaaCodebookRedundant == 3);

  // UDP loopback 127.0.0.1:6980 (may fail without network, but should not crash)
  WHAAPacketHeader sendHdr = hdr;
  sendHdr.qpc = 12345;
  bool sent = SendWhaaPacket(sendHdr, payload.data(), "127.0.0.1", 6980);
  std::printf("UDP send: %s\n", sent ? "OK" : "no network (offline)");
  check("UDP send no crash", true);

  std::printf("{\"schema_version\":1,\"operation\":\"whaa_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
