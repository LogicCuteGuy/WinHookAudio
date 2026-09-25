#include "KsEndpoint.h"

#include <ksmedia.h>

#include "WHAEndpointChannels.h"
#include "WHASlotTable.h"

namespace wha {

namespace {

constexpr REFERENCE_TIME kMinBufferPeriods = 4;  // headroom for Master Clock vs device clock
constexpr REFERENCE_TIME kMinSharedBufferPeriods = 6;  // Windows mixer periods (10 ms each, typically); 3-4 slipped live
constexpr DWORD kStereoMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

WAVEFORMATEXTENSIBLE MakeFormat(int32_t sampleRate, KsSampleFormat format, int channels, DWORD channelMask) {
  const bool isFloat = format == KsSampleFormat::Float32;
  const WORD bits = format == KsSampleFormat::Pcm16 ? 16 : 32;
  WAVEFORMATEXTENSIBLE x{};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = static_cast<WORD>(channels);
  x.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
  x.Format.wBitsPerSample = bits;
  x.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
  x.Format.nAvgBytesPerSec = x.Format.nSamplesPerSec * x.Format.nBlockAlign;
  x.Format.cbSize = 22;
  x.Samples.wValidBitsPerSample = format == KsSampleFormat::Pcm24In32 ? 24 : bits;
  x.dwChannelMask = channelMask;
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

std::string KsResolveEndpointId(EDataFlow flow, const char* endpointId) {
  if (endpointId && *endpointId) return endpointId;
  std::string resolved;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator)))
    return resolved;
  IMMDevice* device = nullptr;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
      char utf8[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, id, -1, utf8, sizeof(utf8), nullptr, nullptr);
      resolved = utf8;
      CoTaskMemFree(id);
    }
    device->Release();
  }
  enumerator->Release();
  return resolved;
}

namespace {

// The endpoint `endpointId` names (empty/null: the Windows default of `flow`); its ID in out.endpointId.
IMMDevice* OpenDevice(EDataFlow flow, const char* endpointId, KsOpenResult& out) {
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
  if (FAILED(hr)) return Fail(out, "MMDeviceEnumerator", hr), nullptr;
  IMMDevice* device = nullptr;
  if (endpointId && *endpointId) {
    wchar_t id[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, endpointId, -1, id, 128);
    hr = enumerator->GetDevice(id, &device);
  } else {
    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  }
  enumerator->Release();
  if (FAILED(hr)) return Fail(out, endpointId && *endpointId ? "GetDevice (endpoint id)" : "GetDefaultAudioEndpoint", hr), nullptr;
  LPWSTR openedId = nullptr;
  if (SUCCEEDED(device->GetId(&openedId)) && openedId) {
    char utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, openedId, -1, utf8, sizeof(utf8), nullptr, nullptr);
    out.endpointId = utf8;
    CoTaskMemFree(openedId);
  }
  return device;
}

void TakeSizes(int32_t sampleRate, REFERENCE_TIME period, KsOpenResult& out) {
  out.periodFrames = static_cast<int32_t>(period * sampleRate / 10000000);
  UINT32 capacity = 0;
  out.client->GetBufferSize(&capacity);
  out.capacityFrames = static_cast<int32_t>(capacity);
  REFERENCE_TIME streamLatency = 0;
  out.client->GetStreamLatency(&streamLatency);
  out.streamLatencyFrames = static_cast<int32_t>(streamLatency * sampleRate / 10000000);
}

// Buffer length in periods: at least `minPeriods` and at least 4 blocks.
REFERENCE_TIME BufferPeriods(REFERENCE_TIME period, int32_t sampleRate, int32_t blockFrames, REFERENCE_TIME minPeriods) {
  const REFERENCE_TIME blockTime = static_cast<REFERENCE_TIME>(blockFrames) * 10000000 / sampleRate;
  REFERENCE_TIME periods = minPeriods;
  while (periods * period < 4 * blockTime) ++periods;
  return periods;
}

bool OpenExclusive(IMMDevice* device, int32_t sampleRate, int32_t periodFrames, int32_t blockFrames, KsOpenResult& out) {
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&out.client);
  if (FAILED(hr)) return Fail(out, "Activate", hr);

  // Period: 0 = device minimum. A request below the minimum gets the minimum: the requested period
  // is a latency wish (the datasheet's HW 64 suits interfaces that allow it), and failing instead
  // left the HW slots silent on common devices (HD Audio minimum 2.67-3 ms).
  REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
  out.client->GetDevicePeriod(&defaultPeriod, &minPeriod);
  // Round up: 128 frames @ 48k = 26666.67 hns must compare equal to a 26667 hns device minimum.
  REFERENCE_TIME period = periodFrames > 0
                              ? (static_cast<REFERENCE_TIME>(periodFrames) * 10000000 + sampleRate - 1) / sampleRate
                              : minPeriod;
  if (period < minPeriod) period = minPeriod;
  const REFERENCE_TIME periods = BufferPeriods(period, sampleRate, blockFrames, kMinBufferPeriods);

  // Channels: all the device has (its Windows device format, with its speaker mask), else stereo.
  // Sample formats differ per device (HD Audio often has no float): the first one it accepts wins.
  DWORD deviceMask = 0;
  int deviceChannels = 0;
  if (IPropertyStore* props = nullptr; SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
    deviceChannels = EndpointChannels(props, &deviceMask);
    props->Release();
  }
  if (deviceChannels > kHwMaxChannels) deviceChannels = kHwMaxChannels, deviceMask = 0;
  struct Layout { int channels; DWORD mask; };
  Layout layouts[2] = {{2, kStereoMask}, {0, 0}};
  if (deviceChannels > 0 && deviceChannels != 2)
    layouts[0] = {deviceChannels, deviceMask}, layouts[1] = {2, kStereoMask};
  else if (deviceChannels == 2 && deviceMask)
    layouts[0].mask = deviceMask;
  const KsSampleFormat candidates[] = {KsSampleFormat::Float32, KsSampleFormat::Pcm24In32, KsSampleFormat::Pcm16};
  WAVEFORMATEXTENSIBLE fmt{};
  bool found = false;
  for (const Layout& l : layouts) {
    if (found || l.channels <= 0) continue;
    for (KsSampleFormat c : candidates) {
      fmt = MakeFormat(sampleRate, c, l.channels, l.mask);
      if (out.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
        out.format = c;
        out.channels = l.channels;
        found = true;
        break;
      }
    }
  }
  if (!found) return Fail(out, "no exclusive format (float32/pcm24in32/pcm16)", AUDCLNT_E_UNSUPPORTED_FORMAT);

  // Timer-driven, so the buffer may be longer than the period.
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
  if (FAILED(hr)) return Fail(out, "Initialize exclusive", hr);
  TakeSizes(sampleRate, period, out);
  return true;
}

// Through the Windows mixer: the mixer's channels, float at the Master Clock's rate (Windows converts
// when the mixer runs at another rate). The period is the mixer's (typically 10 ms); the requested HW
// period does not apply.
bool OpenShared(IMMDevice* device, int32_t sampleRate, int32_t blockFrames, KsOpenResult& out) {
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&out.client);
  if (FAILED(hr)) return Fail(out, "Activate (shared)", hr);
  WAVEFORMATEX* mix = nullptr;
  hr = out.client->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) return Fail(out, "GetMixFormat", FAILED(hr) ? hr : E_POINTER);
  int channels = mix->nChannels;
  DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22
                   ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask
                   : 0;
  CoTaskMemFree(mix);
  if (channels < 1 || channels > kHwMaxChannels) channels = 2, mask = kStereoMask;  // the mixer maps it
  REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
  out.client->GetDevicePeriod(&defaultPeriod, &minPeriod);
  const REFERENCE_TIME period = defaultPeriod > 0 ? defaultPeriod : 100000;
  const REFERENCE_TIME periods = BufferPeriods(period, sampleRate, blockFrames, kMinSharedBufferPeriods);
  const WAVEFORMATEXTENSIBLE fmt = MakeFormat(sampleRate, KsSampleFormat::Float32, channels, mask);
  hr = out.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              period * periods, 0, &fmt.Format, nullptr);
  if (FAILED(hr)) return Fail(out, "Initialize shared", hr);
  out.format = KsSampleFormat::Float32;
  out.channels = channels;
  out.shared = true;
  TakeSizes(sampleRate, period, out);
  return true;
}

}  // namespace

bool KsOpen(EDataFlow flow, const char* endpointId, uint8_t mode, int32_t sampleRate, int32_t periodFrames,
            int32_t blockFrames, KsOpenResult& out) {
  out = KsOpenResult{};
  IMMDevice* device = OpenDevice(flow, endpointId, out);
  if (!device) return false;
  bool ok = false;
  if (mode == HW_MODE_SHARED) {
    ok = OpenShared(device, sampleRate, blockFrames, out);
  } else {
    ok = OpenExclusive(device, sampleRate, periodFrames, blockFrames, out);
    if (!ok && mode == HW_MODE_AUTO) {  // busy, or refuses exclusive mode: through the mixer
      const std::string id = out.endpointId;
      out = KsOpenResult{};
      out.endpointId = id;
      ok = OpenShared(device, sampleRate, blockFrames, out);
    }
  }
  device->Release();
  return ok;
}

}  // namespace wha
