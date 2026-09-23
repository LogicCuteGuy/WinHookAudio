#pragma once

// KsEndpoint — shared exclusive-mode open and sample conversion for the HW output (KsAudio) and
// HW input (KsCapture). Vocabulary: Slot, Master Clock.

#include <cstdint>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace wha {

enum class KsSampleFormat { Float32, Pcm24In32, Pcm16 };  // negotiated per device, first accepted wins

constexpr int kKsDeviceChannels = 2;

struct KsOpenResult {
  IAudioClient* client = nullptr;  // initialized exclusive, not started; caller releases
  KsSampleFormat format = KsSampleFormat::Float32;
  int32_t periodFrames = 0;
  int32_t capacityFrames = 0;       // device buffer
  int32_t streamLatencyFrames = 0;  // IAudioClient::GetStreamLatency
  HRESULT error = S_OK;             // why it failed
  const char* step = "";
};

// Open the endpoint exclusively, timer-driven. endpointId: IMMDevice ID, empty/null = the Windows
// default device of that flow. periodFrames: 0 = device minimum; below the minimum fails. The
// buffer is whole periods, at least 4 of them and at least 4 blockFrames.
bool KsOpenExclusive(EDataFlow flow, const char* endpointId, int32_t sampleRate, int32_t periodFrames,
                     int32_t blockFrames, KsOpenResult& out);

inline float KsClamp(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

inline void KsToDevice(KsSampleFormat format, float v, BYTE* buffer, int index) {
  switch (format) {
    case KsSampleFormat::Float32: reinterpret_cast<float*>(buffer)[index] = v; break;
    case KsSampleFormat::Pcm24In32: reinterpret_cast<int32_t*>(buffer)[index] = static_cast<int32_t>(KsClamp(v) * 8388607.0f) * 256; break;
    case KsSampleFormat::Pcm16: reinterpret_cast<int16_t*>(buffer)[index] = static_cast<int16_t>(KsClamp(v) * 32767.0f); break;
  }
}

inline float KsFromDevice(KsSampleFormat format, const BYTE* buffer, int index) {
  switch (format) {
    case KsSampleFormat::Float32: return reinterpret_cast<const float*>(buffer)[index];
    case KsSampleFormat::Pcm24In32: return static_cast<float>(reinterpret_cast<const int32_t*>(buffer)[index] / 256) / 8388607.0f;
    case KsSampleFormat::Pcm16: return static_cast<float>(reinterpret_cast<const int16_t*>(buffer)[index]) / 32767.0f;
  }
  return 0.0f;
}

}  // namespace wha
