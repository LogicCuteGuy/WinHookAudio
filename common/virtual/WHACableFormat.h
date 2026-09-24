#pragma once

// WHACableFormat - the sample formats a Virtual Cable's Windows streams may use (always stereo) and
// their conversion to and from the cable ring's float frames. Plain C++ without the C++ library, so
// WinHookAudio.sys and an offline test share it.
// Vocabulary: Virtual Cable.

namespace wha {

enum class WHASampleKind : unsigned char { Float32, Pcm16, Pcm24 };

inline unsigned SampleBytes(WHASampleKind k) { return k == WHASampleKind::Pcm16 ? 2u : k == WHASampleKind::Pcm24 ? 3u : 4u; }
inline unsigned FrameBytes(WHASampleKind k) { return 2 * SampleBytes(k); }

// n stereo frames of `kind` (little-endian) -> float frames.
inline void SamplesToFloat(const unsigned char* src, float* dst, unsigned n, WHASampleKind kind) {
  const unsigned count = 2 * n;
  if (kind == WHASampleKind::Pcm16) {
    for (unsigned i = 0; i < count; ++i) {
      const short s = static_cast<short>(src[2 * i] | (src[2 * i + 1] << 8));
      dst[i] = static_cast<float>(s) * (1.0f / 32768.0f);
    }
  } else if (kind == WHASampleKind::Pcm24) {
    for (unsigned i = 0; i < count; ++i) {
      int s = src[3 * i] | (src[3 * i + 1] << 8) | (src[3 * i + 2] << 16);
      if (s & 0x800000) s -= 0x1000000;
      dst[i] = static_cast<float>(s) * (1.0f / 8388608.0f);
    }
  } else {
    const float* f = reinterpret_cast<const float*>(src);
    for (unsigned i = 0; i < count; ++i) dst[i] = f[i];
  }
}

// n float frames -> stereo frames of `kind`; integer formats clip to full scale.
inline void FloatToSamples(const float* src, unsigned char* dst, unsigned n, WHASampleKind kind) {
  const unsigned count = 2 * n;
  if (kind == WHASampleKind::Pcm16) {
    for (unsigned i = 0; i < count; ++i) {
      float x = src[i] * 32768.0f;
      x = x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x;
      const int s = static_cast<int>(x >= 0 ? x + 0.5f : x - 0.5f);
      dst[2 * i] = static_cast<unsigned char>(s & 0xFF);
      dst[2 * i + 1] = static_cast<unsigned char>((s >> 8) & 0xFF);
    }
  } else if (kind == WHASampleKind::Pcm24) {
    for (unsigned i = 0; i < count; ++i) {
      float x = src[i] * 8388608.0f;
      x = x > 8388607.0f ? 8388607.0f : x < -8388608.0f ? -8388608.0f : x;
      const int s = static_cast<int>(x >= 0 ? x + 0.5f : x - 0.5f);
      dst[3 * i] = static_cast<unsigned char>(s & 0xFF);
      dst[3 * i + 1] = static_cast<unsigned char>((s >> 8) & 0xFF);
      dst[3 * i + 2] = static_cast<unsigned char>((s >> 16) & 0xFF);
    }
  } else {
    float* f = reinterpret_cast<float*>(dst);
    for (unsigned i = 0; i < count; ++i) f[i] = src[i];
  }
}

}  // namespace wha
