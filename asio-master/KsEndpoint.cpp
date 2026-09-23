#include "KsEndpoint.h"

#include <ksmedia.h>

namespace wha {

namespace {

constexpr REFERENCE_TIME kMinBufferPeriods = 4;  // headroom for Master Clock vs device clock

WAVEFORMATEXTENSIBLE MakeFormat(int32_t sampleRate, KsSampleFormat format) {
  const bool isFloat = format == KsSampleFormat::Float32;
  const WORD bits = format == KsSampleFormat::Pcm16 ? 16 : 32;
  WAVEFORMATEXTENSIBLE x{};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = kKsDeviceChannels;
  x.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
  x.Format.wBitsPerSample = bits;
  x.Format.nBlockAlign = static_cast<WORD>(kKsDeviceChannels * bits / 8);
  x.Format.nAvgBytesPerSec = x.Format.nSamplesPerSec * x.Format.nBlockAlign;
  x.Format.cbSize = 22;
  x.Samples.wValidBitsPerSample = format == KsSampleFormat::Pcm24In32 ? 24 : bits;
  x.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  x.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return x;
}

bool Fail(KsOpenResult& out, const char* step, HRESULT hr) {
  if (out.client) { out.client->Release(); out.client = nullptr; }
  out.error = hr;
  out.step = step;
  return false;
}

}  // namespace

bool KsOpenExclusive(EDataFlow flow, const char* endpointId, int32_t sampleRate, int32_t periodFrames,
                     int32_t blockFrames, KsOpenResult& out) {
  out = KsOpenResult{};
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
  if (FAILED(hr)) return Fail(out, "MMDeviceEnumerator", hr);

  IMMDevice* device = nullptr;
  if (endpointId && *endpointId) {
    wchar_t id[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, endpointId, -1, id, 128);
    hr = enumerator->GetDevice(id, &device);
  } else {
    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  }
  enumerator->Release();
  if (FAILED(hr)) return Fail(out, endpointId && *endpointId ? "GetDevice (endpoint id)" : "GetDefaultAudioEndpoint", hr);

  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&out.client);
  if (FAILED(hr)) { device->Release(); return Fail(out, "Activate", hr); }

  // Period: 0 = device minimum; a smaller request than the device allows is rejected, not stretched.
  REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
  out.client->GetDevicePeriod(&defaultPeriod, &minPeriod);
  // Round up: 128 frames @ 48k = 26666.67 hns must compare equal to a 26667 hns device minimum.
  REFERENCE_TIME period = periodFrames > 0
                              ? (static_cast<REFERENCE_TIME>(periodFrames) * 10000000 + sampleRate - 1) / sampleRate
                              : minPeriod;
  if (period < minPeriod) { device->Release(); return Fail(out, "period below device minimum", AUDCLNT_E_INVALID_DEVICE_PERIOD); }
  const REFERENCE_TIME blockTime = static_cast<REFERENCE_TIME>(blockFrames) * 10000000 / sampleRate;
  REFERENCE_TIME periods = kMinBufferPeriods;
  while (periods * period < 4 * blockTime) ++periods;

  // Exclusive formats differ per device (HD Audio often has no float): take the first one it accepts.
  const KsSampleFormat candidates[] = {KsSampleFormat::Float32, KsSampleFormat::Pcm24In32, KsSampleFormat::Pcm16};
  WAVEFORMATEXTENSIBLE fmt{};
  bool found = false;
  for (KsSampleFormat c : candidates) {
    fmt = MakeFormat(sampleRate, c);
    if (out.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
      out.format = c;
      found = true;
      break;
    }
  }
  if (!found) { device->Release(); return Fail(out, "no exclusive format (float32/pcm24in32/pcm16)", AUDCLNT_E_UNSUPPORTED_FORMAT); }

  // Exclusive only — no shared fallback (busy pin must surface as failure). Timer-driven, so the
  // buffer may be longer than the period.
  hr = out.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, period * periods, period, &fmt.Format, nullptr);
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {  // re-open with the device-aligned period
    UINT32 aligned = 0;
    out.client->GetBufferSize(&aligned);
    period = static_cast<REFERENCE_TIME>(10000000.0 * aligned / static_cast<double>(periods) / sampleRate + 0.5);
    out.client->Release();
    out.client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&out.client);
    if (SUCCEEDED(hr)) hr = out.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, period * periods, period, &fmt.Format, nullptr);
  }
  device->Release();
  if (FAILED(hr)) return Fail(out, "Initialize exclusive", hr);

  out.periodFrames = static_cast<int32_t>(period * sampleRate / 10000000);
  UINT32 capacity = 0;
  out.client->GetBufferSize(&capacity);
  out.capacityFrames = static_cast<int32_t>(capacity);
  REFERENCE_TIME streamLatency = 0;
  out.client->GetStreamLatency(&streamLatency);
  out.streamLatencyFrames = static_cast<int32_t>(streamLatency * sampleRate / 10000000);
  return true;
}

}  // namespace wha
