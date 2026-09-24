// cable-live — live check of WinHookAudio.sys (the Virtual Cable driver) without a DAW.
// Plays a tone into the cable's Windows playback endpoint (WASAPI shared) and reads it back through
// the driver's control device, as the Worker does; sends another tone through the control device and
// records it from the cable's Windows recording endpoint. Needs the driver installed and no Worker
// running (the control device takes one client). Not a ctest: run it by hand.
//   winhookaudio-cable-live.exe [--seconds N] [--block FRAMES] [--cable 1..8]

#include <windows.h>
#include <winioctl.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>  // after mmdeviceapi.h (DEFINE_PROPERTYKEY)
#include <timeapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "virtual/WHACableProtocol.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPlayHz = 440.0;    // Windows -> cable -> "Worker"
constexpr double kRecordHz = 1000.0; // "Worker" -> cable -> Windows
constexpr float kAmplitude = 0.5f;
unsigned gCable = 1;  // 1-based, as the endpoints are named

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("  %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

template <class T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// The active endpoint in direction `flow` named "WinHookAudio Output <gCable>" / "... Input <gCable>".
IMMDevice* FindEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow, std::wstring& name) {
  wchar_t want[64];
  swprintf_s(want, L"WinHookAudio %s %u (", flow == eRender ? L"Output" : L"Input", gCable);
  IMMDeviceCollection* all = nullptr;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all))) return nullptr;
  UINT count = 0;
  all->GetCount(&count);
  IMMDevice* found = nullptr;
  for (UINT i = 0; i < count && !found; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(all->Item(i, &device))) continue;
    IPropertyStore* props = nullptr;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
        SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR &&
        std::wcsstr(v.pwszVal, want) == v.pwszVal) {
      name = v.pwszVal;
      found = device;
      device = nullptr;
    }
    PropVariantClear(&v);
    SafeRelease(props);
    SafeRelease(device);
  }
  SafeRelease(all);
  return found;
}

// Amplitude of the `hz` component of a mono signal (Goertzel over the whole span).
double ToneAmplitude(const std::vector<float>& x, size_t begin, size_t end, double hz, double rate) {
  if (end <= begin) return 0;
  const double w = 2 * kPi * hz / rate, coeff = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = begin; i < end; ++i) {
    const double s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  return 2 * std::sqrt(power > 0 ? power : 0) / static_cast<double>(end - begin);
}

// Blocks of `block` frames from frame `from` on, after the first loud one, whose RMS falls below a
// quarter of the tone's; `at` gets their start frames.
int Dropouts(const std::vector<float>& x, size_t from, size_t block, std::vector<size_t>* at = nullptr) {
  int dropouts = 0;
  bool started = false;
  for (size_t b = from; b + block <= x.size(); b += block) {
    double e = 0;
    for (size_t i = b; i < b + block; ++i) e += x[i] * x[i];
    const double rms = std::sqrt(e / static_cast<double>(block));
    if (rms > kAmplitude * 0.5) started = true;
    else if (started && rms < kAmplitude * 0.707 * 0.25) {
      ++dropouts;
      if (at) at->push_back(b);
    }
  }
  return dropouts;
}

bool Exchange(HANDLE device, std::vector<unsigned char>& io, unsigned frames, const float* record,
              WHACableExchange& reply, std::vector<float>* playOut) {
  auto* h = reinterpret_cast<WHACableExchange*>(io.data());
  std::memset(h, 0, sizeof(*h));
  h->protocol = WHA_CABLE_PROTOCOL;
  h->cable = gCable - 1;
  h->frames = frames;
  h->hasRecord = record ? 1u : 0u;
  float* audio = reinterpret_cast<float*>(io.data() + sizeof(WHACableExchange));
  const DWORD bytes = static_cast<DWORD>(sizeof(WHACableExchange) + frames * 2 * sizeof(float));
  if (record) std::memcpy(audio, record, frames * 2 * sizeof(float));
  DWORD got = 0;
  if (!DeviceIoControl(device, IOCTL_WHA_CABLE_EXCHANGE, io.data(), record ? bytes : sizeof(WHACableExchange),
                       io.data(), bytes, &got, nullptr) || got != bytes)
    return false;
  std::memcpy(&reply, h, sizeof(reply));
  if (playOut)
    for (unsigned i = 0; i < frames; ++i) playOut->push_back(audio[2 * i]);  // left
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 10;
  unsigned block = 256;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--block")) block = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--cable")) gCable = static_cast<unsigned>(std::atoi(argv[++i]));
  }
  if (block < 16 || block > WHA_CABLE_MAX_FRAMES) block = 256;
  if (gCable < 1 || gCable > 8) gCable = 1;
  std::printf("cable-live: cable %u, %.0f s, Worker block %u\n", gCable, seconds, block);

  HANDLE device = CreateFileW(WHA_CABLE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (device == INVALID_HANDLE_VALUE) {
    std::printf("  control device %ls: error %lu (driver not installed, or a Worker has it open)\n",
                WHA_CABLE_USER_PATH, GetLastError());
    check("control device opens", false);
    return 1;
  }
  std::vector<unsigned char> io(sizeof(WHACableExchange) + WHA_CABLE_MAX_FRAMES * 2 * sizeof(float));
  WHACableExchange reply{};
  const bool probed = Exchange(device, io, 0, nullptr, reply, nullptr);
  check("control device answers (protocol 1)", probed && reply.protocol == WHA_CABLE_PROTOCOL);
  if (!probed) return 1;
  std::printf("  driver: %u cable(s)\n", reply.cables);

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  std::wstring renderName, captureName;
  IMMDevice* renderDevice = enumerator ? FindEndpoint(enumerator, eRender, renderName) : nullptr;
  IMMDevice* captureDevice = enumerator ? FindEndpoint(enumerator, eCapture, captureName) : nullptr;
  std::printf("  playback endpoint: %ls\n  recording endpoint: %ls\n", renderDevice ? renderName.c_str() : L"(none)",
              captureDevice ? captureName.c_str() : L"(none)");
  check("both Windows endpoints exist", renderDevice && captureDevice);
  if (!renderDevice || !captureDevice) return 1;

  IAudioClient *render = nullptr, *capture = nullptr;
  renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&render));
  captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&capture));
  WAVEFORMATEX *renderFormat = nullptr, *captureFormat = nullptr;
  render->GetMixFormat(&renderFormat);
  capture->GetMixFormat(&captureFormat);
  std::printf("  formats: playback %lu Hz %u ch %u bit, recording %lu Hz %u ch %u bit\n", renderFormat->nSamplesPerSec,
              renderFormat->nChannels, renderFormat->wBitsPerSample, captureFormat->nSamplesPerSec,
              captureFormat->nChannels, captureFormat->wBitsPerSample);
  const double rate = renderFormat->nSamplesPerSec;
  const bool floatStereo = renderFormat->nChannels == 2 && renderFormat->wBitsPerSample == 32 &&
                           captureFormat->nChannels == 2 && captureFormat->wBitsPerSample == 32;
  check("float stereo on both endpoints", floatStereo);
  if (!floatStereo) return 1;
  const REFERENCE_TIME bufferTime = 1000000;  // 100 ms
  HRESULT hr = render->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferTime, 0, renderFormat, nullptr);
  if (SUCCEEDED(hr)) hr = capture->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferTime, 0, captureFormat, nullptr);
  check("WASAPI shared streams open", SUCCEEDED(hr));
  if (FAILED(hr)) {
    std::printf("  HRESULT 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }
  IAudioRenderClient* renderClient = nullptr;
  IAudioCaptureClient* captureClient = nullptr;
  render->GetService(IID_PPV_ARGS(&renderClient));
  capture->GetService(IID_PPV_ARGS(&captureClient));
  UINT32 renderFrames = 0;
  render->GetBufferSize(&renderFrames);
  render->Start();
  capture->Start();

  // Worker side: one exchange per block at the playback rate, paced by the performance counter. A 1 ms
  // timer makes Sleep(1) about 1 ms (not 15.6), so blocks go out one at a time like the real Worker's
  // ASIO-driven ticks rather than in bursts.
  timeBeginPeriod(1);
  std::vector<float> fromWindows, toWindows, recordBlock(2 * block);
  LARGE_INTEGER freq, start, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  long long exchanged = 0, played = 0, recordPhase = 0;
  int exchangeErrors = 0;
  unsigned seenPlayRate = 0, seenRecordRate = 0;
  for (;;) {
    QueryPerformanceCounter(&now);
    const double t = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
    if (t >= seconds) break;
    // Windows app: keep the playback buffer full with the 440 Hz tone.
    UINT32 padding = 0;
    render->GetCurrentPadding(&padding);
    if (renderFrames > padding) {
      BYTE* data = nullptr;
      const UINT32 n = renderFrames - padding;
      if (SUCCEEDED(renderClient->GetBuffer(n, &data))) {
        auto* f = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < n; ++i, ++played)
          f[2 * i] = f[2 * i + 1] = kAmplitude * static_cast<float>(std::sin(2 * kPi * kPlayHz * played / rate));
        renderClient->ReleaseBuffer(n, 0);
      }
    }
    // Windows app: record.
    UINT32 packet = 0;
    while (SUCCEEDED(captureClient->GetNextPacketSize(&packet)) && packet) {
      BYTE* data = nullptr;
      UINT32 n = 0;
      DWORD flags = 0;
      if (FAILED(captureClient->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
      auto* f = reinterpret_cast<const float*>(data);
      for (UINT32 i = 0; i < n; ++i) toWindows.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : f[2 * i]);
      captureClient->ReleaseBuffer(n);
    }
    // "Worker": exchange blocks that are due.
    while (static_cast<double>(exchanged + block) <= t * rate) {
      for (unsigned i = 0; i < block; ++i, ++recordPhase)
        recordBlock[2 * i] = recordBlock[2 * i + 1] =
            kAmplitude * static_cast<float>(std::sin(2 * kPi * kRecordHz * recordPhase / rate));
      if (!Exchange(device, io, block, recordBlock.data(), reply, &fromWindows)) ++exchangeErrors;
      if (reply.playRate) seenPlayRate = reply.playRate;
      if (reply.recordRate) seenRecordRate = reply.recordRate;
      exchanged += block;
    }
    Sleep(1);
  }
  timeEndPeriod(1);
  render->Stop();
  capture->Stop();

  std::printf("  driver saw: playback %u Hz, recording %u Hz; playback underruns %u drops %u, recording underruns %u drops %u\n",
              seenPlayRate, seenRecordRate, reply.playUnderruns, reply.playDrops, reply.recordUnderruns, reply.recordDrops);
  check("no exchange errors", exchangeErrors == 0);
  check("driver reports both streams running at the endpoint rate",
        seenPlayRate == renderFormat->nSamplesPerSec && seenRecordRate == captureFormat->nSamplesPerSec);
  // Skip the first second (streams starting, rings priming).
  const size_t skipFrom = static_cast<size_t>(rate);
  const double playAmp = ToneAmplitude(fromWindows, skipFrom, fromWindows.size(), kPlayHz, rate);
  const double recordAmp = ToneAmplitude(toWindows, skipFrom, toWindows.size(), kRecordHz, rate);
  std::vector<size_t> playAt, recordAt;
  const int playDropouts = Dropouts(fromWindows, skipFrom, block, &playAt);
  const int recordDropouts = Dropouts(toWindows, skipFrom, 480, &recordAt);
  std::printf("  Windows -> cable -> Worker: %zu frames, 440 Hz amplitude %.3f (sent %.3f), %d dropout blocks\n",
              fromWindows.size(), playAmp, kAmplitude, playDropouts);
  std::printf("  Worker -> cable -> Windows: %zu frames, 1 kHz amplitude %.3f (sent %.3f), %d dropout blocks\n",
              toWindows.size(), recordAmp, kAmplitude, recordDropouts);
  check("Windows playback reaches the Worker (440 Hz within 10%)", std::fabs(playAmp - kAmplitude) < 0.1 * kAmplitude);
  check("Worker audio reaches Windows recording (1 kHz within 10%)", std::fabs(recordAmp - kAmplitude) < 0.1 * kAmplitude);
  for (size_t f : playAt) std::printf("    Windows -> Worker dropout at %.3f s\n", static_cast<double>(f) / rate);
  for (size_t f : recordAt) std::printf("    Worker -> Windows dropout at %.3f s\n", static_cast<double>(f) / rate);
  check("no dropouts after the first second", playDropouts == 0 && recordDropouts == 0);

  CoTaskMemFree(renderFormat);
  CoTaskMemFree(captureFormat);
  SafeRelease(renderClient);
  SafeRelease(captureClient);
  SafeRelease(render);
  SafeRelease(capture);
  SafeRelease(renderDevice);
  SafeRelease(captureDevice);
  SafeRelease(enumerator);
  CloseHandle(device);
  CoUninitialize();
  std::printf("%s\n", gPass ? "ALL PASS" : "SOME FAILED");
  return gPass ? 0 : 1;
}
