#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
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

  check("CODEBOOK 3x redundant", kWhaaCodebookRedundant == 3);

  // 1 s of 1 kHz sine @ 48k stereo, amplitude 0.5, fed in 128-frame Master Clock blocks
  constexpr uint32_t kRate = 48000, kCh = 2, kBlock = 128;
  const double kPi = 3.14159265358979323846;
  std::vector<float> sine(kRate * kCh);
  for (uint32_t f = 0; f < kRate; ++f)
    for (uint32_t c = 0; c < kCh; ++c) sine[f * kCh + c] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * f / kRate));

  // Vorbis (15): real libvorbis round trip, or explicit unavailability when not vendored
  const bool vorbis = VorbisAvailable();
  WHAVorbisEncoder enc;
  WHACodebook cb;
  if (vorbis) {
    check("Vorbis encoder open Q0.4", enc.Open(0, kRate, kCh, 0.4f, cb));
    check("CODEBOOK valid", ValidateCodebook(cb));
    check("CODEBOOK ident header", cb.headers.size() > 19 && cb.headers[12] == 0x01 && std::memcmp(&cb.headers[13], "vorbis", 6) == 0);
    std::vector<std::vector<uint8_t>> packets;
    bool encOk = true;
    for (uint32_t f = 0; f < kRate; f += kBlock) encOk = encOk && enc.Encode(&sine[f * kCh], kBlock, packets);
    check("Vorbis encode blocks", encOk && !packets.empty());
    check("Vorbis flush", enc.Flush(packets));
    bool fits = true;
    for (auto& p : packets) fits = fits && p.size() <= kWhaaMaxPayload;
    check("Vorbis packets fit WHAA payload", fits);

    // Bitrate: VBR spends almost nothing on a pure sine, so measure on 1 s of broadband noise
    std::vector<float> noiseIn(kRate * kCh);
    uint32_t lcg = 1;
    for (float& s : noiseIn) { lcg = lcg * 1664525u + 1013904223u; s = 0.25f * (static_cast<int32_t>(lcg) / 2147483648.0f); }
    WHAVorbisEncoder noiseEnc;
    WHACodebook noiseCb;
    std::vector<std::vector<uint8_t>> noisePackets;
    bool noiseOk = noiseEnc.Open(1, kRate, kCh, 0.4f, noiseCb);
    for (uint32_t f = 0; noiseOk && f < kRate; f += kBlock) noiseOk = noiseEnc.Encode(&noiseIn[f * kCh], kBlock, noisePackets);
    noiseOk = noiseOk && noiseEnc.Flush(noisePackets);
    size_t bytes = 0;
    for (auto& p : noisePackets) bytes += p.size();
    const double kbps = bytes * 8.0 / 1000.0;
    std::printf("Vorbis Q0.4 stereo noise: %zu packets, %.1f kbps\n", noisePackets.size(), kbps);
    check("Vorbis bitrate 64-500 kbps", noiseOk && kbps >= 64.0 && kbps <= 500.0);

    WHAVorbisDecoder dec;
    check("Vorbis decoder open", dec.Open(cb));
    std::vector<float> decoded;
    bool decOk = true;
    for (auto& p : packets) decOk = decOk && dec.Decode(p.data(), static_cast<uint32_t>(p.size()), decoded);
    const size_t decodedFrames = decoded.size() / kCh;
    std::printf("Vorbis decoded frames: %zu\n", decodedFrames);
    check("Vorbis decode", decOk && decodedFrames >= kRate - 2048 && decodedFrames <= kRate + 2048);
    // Error vs original over the steady middle second-half (skips priming at the edges)
    double noise = 0, ref = 0;
    for (size_t i = (kRate / 4) * kCh; i < (kRate * 3 / 4) * kCh && i < decoded.size(); ++i) {
      noise += (decoded[i] - sine[i]) * (decoded[i] - sine[i]);
      ref += sine[i] * sine[i];
    }
    const double snrDb = noise > 0 ? 10.0 * std::log10(ref / noise) : 200.0;
    std::printf("Vorbis Q0.4 SNR: %.1f dB\n", snrDb);
    check("Vorbis SNR >= 20 dB", snrDb >= 20.0);

    // Rejections: malformed CODEBOOK, mismatched fields, non-audio packet
    WHACodebook bad = cb;
    bad.headers.resize(bad.headers.size() - 1);
    check("CODEBOOK truncated rejected", !ValidateCodebook(bad) && !WHAVorbisDecoder().Open(bad));
    bad = cb;
    bad.channels = 1;
    check("CODEBOOK channel mismatch rejected", !WHAVorbisDecoder().Open(bad));
    bad = cb;
    bad.headers[13] = 'X';
    check("CODEBOOK corrupt header rejected", !WHAVorbisDecoder().Open(bad));
    std::vector<float> none;
    check("Vorbis header-as-audio rejected", !dec.Decode(&cb.headers[12], 30, none));
    check("Vorbis encoder rejects Q1.5", !WHAVorbisEncoder().Open(0, kRate, kCh, 1.5f, bad));
  } else {
    check("Vorbis unavailable reports open failure", !enc.Open(0, kRate, kCh, 0.4f, cb));
  }

  // r8brain SRC (15): 48k -> 96k / 44.1k / 48k keep the 0.5 sine amplitude
  const bool r8b = ResamplerAvailable();
  if (r8b) {
    auto runSrc = [&](uint32_t outRate, std::vector<float>& out) {
      WHAResampler src;
      if (!src.Open(kRate, outRate, kCh, kBlock)) return false;
      for (uint32_t f = 0; f < kRate; f += kBlock)
        if (!src.Process(&sine[f * kCh], kBlock, out)) return false;
      return true;
    };
    for (uint32_t outRate : {96000u, 44100u, 48000u}) {
      std::vector<float> srcOut;
      const bool ok = runSrc(outRate, srcOut);
      const size_t frames = srcOut.size() / kCh;
      float peak = 0;
      for (size_t i = (frames / 4) * kCh; i < (frames * 3 / 4) * kCh; ++i) peak = std::max(peak, std::abs(srcOut[i]));
      std::printf("SRC 48000->%u: %zu frames, peak %.4f\n", outRate, frames, peak);
      char name[64];
      std::snprintf(name, sizeof(name), "SRC 48000->%u", outRate);
      check(name, ok && frames + 4096 >= outRate && frames <= outRate && std::abs(peak - 0.5f) < 0.01f);
    }
    WHAResampler src;
    std::vector<float> unused;
    check("SRC rejects oversize block", src.Open(kRate, 96000, kCh, kBlock) && !src.Process(sine.data(), kBlock + 1, unused));
  } else {
    WHAResampler src;
    check("SRC unavailable reports open failure", !src.Open(kRate, 96000, kCh, kBlock));
  }

  // Jitter Vorbis 50ms
  WHAJitterBuffer jitterVorbis(50, 48000);
  jitterVorbis.push(hdr, payload.data(), static_cast<uint32_t>(payload.size()), 1000);
  check("jitter Vorbis 50ms", jitterVorbis.jitterMs() == 50);

  // UDP loopback 127.0.0.1:6980 (may fail without network, but should not crash)
  WHAAPacketHeader sendHdr = hdr;
  sendHdr.qpc = 12345;
  bool sent = SendWhaaPacket(sendHdr, payload.data(), "127.0.0.1", 6980);
  std::printf("UDP send: %s\n", sent ? "OK" : "no network (offline)");
  check("UDP send no crash", true);

  std::printf("{\"schema_version\":1,\"operation\":\"whaa_test\",\"stream_verified\":false,\"vorbis\":%s,\"r8brain\":%s,\"pass\":%s}\n",
              vorbis ? "true" : "false", r8b ? "true" : "false", pass ? "true" : "false");
  return pass ? 0 : 1;
}
