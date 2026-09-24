#pragma once

// WHACableFormat - the sample formats a Virtual Cable's Windows streams may use (1..8 channels, 16 to
// 32 bits) and their conversion to and from the cable ring's float samples. Plain C++ without the C++
// library, so WinHookAudio.sys and an offline test share it.
// Vocabulary: Virtual Cable.

namespace wha {

constexpr unsigned kCableChannels = 8;  // most channels a cable carries (7.1)

// The number is what the Slot Table stores (WHACableSetting::format) and the Worker sends the driver
// (WHACableExchange::format): do not renumber.
enum class WHASampleKind : unsigned char { Float32 = 0, Pcm16 = 1, Pcm24 = 2, Pcm32 = 3, Pcm24in32 = 4 };
constexpr unsigned kSampleKinds = 5;

constexpr bool IsValidCableFormat(unsigned format) { return format < kSampleKinds; }
// Stereo, quad, 5.1, 7.1: the channel counts Windows has a speaker setup for.
constexpr bool IsValidCableChannels(unsigned channels) {
  return channels == 2 || channels == 4 || channels == 6 || channels == 8;
}

// Bytes one sample takes in the Windows buffer (the container), and the bits of it that carry audio.
constexpr unsigned SampleBytes(WHASampleKind k) { return k == WHASampleKind::Pcm16 ? 2u : k == WHASampleKind::Pcm24 ? 3u : 4u; }
constexpr unsigned ValidBits(WHASampleKind k) {
  return k == WHASampleKind::Pcm16 ? 16u : k == WHASampleKind::Pcm24 || k == WHASampleKind::Pcm24in32 ? 24u : 32u;
}
constexpr unsigned FrameBytes(WHASampleKind k, unsigned channels) { return channels * SampleBytes(k); }

// The speakers of a cable with `channels` channels (WAVEFORMATEXTENSIBLE dwChannelMask, KSAUDIO_SPEAKER_*):
// stereo, quad (FL FR BL BR), 5.1 (FL FR FC LFE BL BR), 7.1 surround (+ SL SR). 0: no standard setup.
constexpr unsigned CableChannelMask(unsigned channels) {
  return channels == 2 ? 0x3u : channels == 4 ? 0x33u : channels == 6 ? 0x3Fu : channels == 8 ? 0x63Fu : 0u;
}

// Short speaker name of channel `index` (0-based) of a cable with `channels` channels: "L", "R", "C",
// "LFE", "BL", "BR", "SL", "SR"; nullptr out of range.
inline const char* CableChannelName(unsigned channels, unsigned index) {
  static const char* const kSurround[kCableChannels] = {"L", "R", "C", "LFE", "BL", "BR", "SL", "SR"};
  static const char* const kQuad[4] = {"L", "R", "BL", "BR"};
  if (index >= channels || index >= kCableChannels) return nullptr;
  return channels == 4 ? kQuad[index] : kSurround[index];
}

// `samples` samples of `kind` (little-endian, interleaved) -> float.
inline void SamplesToFloat(const unsigned char* src, float* dst, unsigned samples, WHASampleKind kind) {
  if (kind == WHASampleKind::Pcm16) {
    for (unsigned i = 0; i < samples; ++i) {
      const short s = static_cast<short>(src[2 * i] | (src[2 * i + 1] << 8));
      dst[i] = static_cast<float>(s) * (1.0f / 32768.0f);
    }
  } else if (kind == WHASampleKind::Pcm24) {
    for (unsigned i = 0; i < samples; ++i) {
      int s = src[3 * i] | (src[3 * i + 1] << 8) | (src[3 * i + 2] << 16);
      if (s & 0x800000) s -= 0x1000000;
      dst[i] = static_cast<float>(s) * (1.0f / 8388608.0f);
    }
  } else if (kind == WHASampleKind::Pcm32 || kind == WHASampleKind::Pcm24in32) {  // 24-in-32: low byte 0
    for (unsigned i = 0; i < samples; ++i) {
      const int s = static_cast<int>(static_cast<unsigned>(src[4 * i]) | (static_cast<unsigned>(src[4 * i + 1]) << 8) |
                                     (static_cast<unsigned>(src[4 * i + 2]) << 16) | (static_cast<unsigned>(src[4 * i + 3]) << 24));
      dst[i] = static_cast<float>(s) * (1.0f / 2147483648.0f);
    }
  } else {
    const float* f = reinterpret_cast<const float*>(src);
    for (unsigned i = 0; i < samples; ++i) dst[i] = f[i];
  }
}

// `samples` float samples -> `kind`; integer formats clip to full scale.
inline void FloatToSamples(const float* src, unsigned char* dst, unsigned samples, WHASampleKind kind) {
  if (kind == WHASampleKind::Pcm16) {
    for (unsigned i = 0; i < samples; ++i) {
      float x = src[i] * 32768.0f;
      x = x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x;
      const int s = static_cast<int>(x >= 0 ? x + 0.5f : x - 0.5f);
      dst[2 * i] = static_cast<unsigned char>(s & 0xFF);
      dst[2 * i + 1] = static_cast<unsigned char>((s >> 8) & 0xFF);
    }
  } else if (kind == WHASampleKind::Pcm24 || kind == WHASampleKind::Pcm24in32) {
    const unsigned step = SampleBytes(kind), low = step - 3;  // 24-in-32: a zero low byte, then the 24 bits
    for (unsigned i = 0; i < samples; ++i) {
      float x = src[i] * 8388608.0f;
      x = x > 8388607.0f ? 8388607.0f : x < -8388608.0f ? -8388608.0f : x;
      const int s = static_cast<int>(x >= 0 ? x + 0.5f : x - 0.5f);
      unsigned char* d = dst + step * i;
      if (low) d[0] = 0;
      d[low] = static_cast<unsigned char>(s & 0xFF);
      d[low + 1] = static_cast<unsigned char>((s >> 8) & 0xFF);
      d[low + 2] = static_cast<unsigned char>((s >> 16) & 0xFF);
    }
  } else if (kind == WHASampleKind::Pcm32) {
    for (unsigned i = 0; i < samples; ++i) {
      double x = static_cast<double>(src[i]) * 2147483648.0;
      x = x > 2147483647.0 ? 2147483647.0 : x < -2147483648.0 ? -2147483648.0 : x;
      const unsigned s = static_cast<unsigned>(static_cast<int>(x >= 0 ? x + 0.5 : x - 0.5));
      dst[4 * i] = static_cast<unsigned char>(s & 0xFF);
      dst[4 * i + 1] = static_cast<unsigned char>((s >> 8) & 0xFF);
      dst[4 * i + 2] = static_cast<unsigned char>((s >> 16) & 0xFF);
      dst[4 * i + 3] = static_cast<unsigned char>((s >> 24) & 0xFF);
    }
  } else {
    float* f = reinterpret_cast<float*>(dst);
    for (unsigned i = 0; i < samples; ++i) f[i] = src[i];
  }
}

}  // namespace wha
