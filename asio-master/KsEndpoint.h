#pragma once

// KsEndpoint â€” WASAPI open (Exclusive / Shared / Auto, WHAHwMode) and sample conversion for the HW
// output (KsAudio) and HW input (KsCapture). Vocabulary: Slot, Master Clock.

#include <cstdint>
#include <string>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "WHASlotTable.h"

namespace wha {

enum class KsSampleFormat { Float32, Pcm24In32, Pcm16 };  // negotiated per device, first accepted wins

struct KsOpenResult {
  IAudioClient* client = nullptr;  // initialized, not started; caller releases
  KsSampleFormat format = KsSampleFormat::Float32;
  int32_t channels = 2;             // the device's channel count, or 2 when it refused that (stereo)
  bool shared = false;              // opened through the Windows mixer (Shared, or Auto's fallback)
  int32_t periodFrames = 0;         // device period (Shared: the mixer's)
  int32_t capacityFrames = 0;       // device buffer
  int32_t streamLatencyFrames = 0;  // IAudioClient::GetStreamLatency
  std::string endpointId;           // IMMDevice ID actually opened (UTF-8), also for the default device
  HRESULT error = S_OK;             // why it failed
  const char* step = "";
};

// Open the endpoint in `mode` (WHAHwMode), timer-driven. endpointId: IMMDevice ID, empty/null = the
// Windows default device of that flow. The buffer is whole periods and at least 4 blockFrames.
// Exclusive: periodFrames 0 or below the device minimum = the minimum; at least 4 periods; channels:
// all the device has (its Windows device format, up to kHwMaxChannels), else stereo. Shared: the
// Windows mixer's period (periodFrames does not apply), at least 6 periods, the mixer's channels,
// float; Windows converts the rate if the mixer runs at another. Auto: Exclusive, else Shared.
bool KsOpen(EDataFlow flow, const char* endpointId, uint8_t mode, int32_t sampleRate, int32_t periodFrames,
            int32_t blockFrames, KsOpenResult& out);

// The endpoint an ID opens: the ID itself, or for empty/null the Windows default device of that flow
// ("" if there is none). Two HW devices of the Slot Table that resolve alike are one device.
std::string KsResolveEndpointId(EDataFlow flow, const char* endpointId);

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
