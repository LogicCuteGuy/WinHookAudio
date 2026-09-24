// cable-live — live check of WinHookAudio.sys (the Virtual Cable driver) without a DAW.
// Plays a tone into the cable's Windows playback endpoint (WASAPI shared) and reads it back through
// the driver's control device, as the Worker does; sends another tone through the control device and
// records it from the cable's Windows recording endpoint. First it sends the cable's format (as the
// Worker does from the panel's settings) and sets both endpoints to it with CableEndpointSync. Needs
// the driver installed and no Worker running (the control device takes one client). Not a ctest.
//   winhookaudio-cable-live.exe [--seconds N] [--block FRAMES] [--cable 1..8]
//                               [--rate HZ] [--channels 2|4|6|8] [--format 0..4]
// format: 0 = 32-bit float, 1 = 16-bit, 2 = 24-bit, 3 = 32-bit, 4 = 24 bits in 32.

#include <windows.h>
#include <winioctl.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>  // after mmdeviceapi.h (DEFINE_PROPERTYKEY)
#include <ksmedia.h>                        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#include <timeapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "CableEndpointSync.h"
#include "virtual/WHACableProtocol.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPlayHz = 440.0;    // Windows -> cable -> "Worker"
constexpr double kRecordHz = 1000.0; // "Worker" -> cable -> Windows
constexpr float kAmplitude = 0.5f;
unsigned gCable = 1;  // 1-based, as the endpoints are named
unsigned gRate = 48000, gChannels = 2, gFormat = 0;  // the cable's format, sent with every exchange

// PKEY_AudioEngine_DeviceFormat: the format the audio engine opens the device in (a WAVEFORMATEX blob).
const PROPERTYKEY kDeviceFormatKey = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

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

// The engine's device format of `device`: rate, channels, container and valid bits, float or not.
struct DeviceFormat {
  unsigned rate = 0, channels = 0, bits = 0, validBits = 0;
  bool isFloat = false;
};
bool ReadDeviceFormat(IMMDevice* device, DeviceFormat& out) {
  IPropertyStore* props = nullptr;
  PROPVARIANT v;
  PropVariantInit(&v);
  bool ok = false;
  if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(kDeviceFormatKey, &v)) &&
      v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
    const auto* w = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
    out.rate = w->nSamplesPerSec;
    out.channels = w->nChannels;
    out.bits = out.validBits = w->wBitsPerSample;
    out.isFloat = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && v.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
      const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
      out.validBits = x->Samples.wValidBitsPerSample;
      out.isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    ok = true;
  }
  PropVariantClear(&v);
  SafeRelease(props);
  return ok;
}
// What gFormat means on the device: container bits, valid bits, float.
bool WantedFormat(const DeviceFormat& f) {
  static const unsigned kBits[] = {32, 16, 24, 32, 32}, kValid[] = {32, 16, 24, 32, 24};
  return f.rate == gRate && f.channels == gChannels && f.bits == kBits[gFormat] && f.validBits == kValid[gFormat] &&
         f.isFloat == (gFormat == 0);
}
void PrintFormat(const char* what, const DeviceFormat& f) {
  std::printf("  %s: %u Hz, %u ch, %u bit (%u valid)%s\n", what, f.rate, f.channels, f.bits, f.validBits,
              f.isFloat ? " float" : "");
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

// One exchange in the cable's format (probe: rate 0, only asks for the header).
bool Exchange(HANDLE device, std::vector<unsigned char>& io, unsigned frames, const float* record,
              WHACableExchange& reply, std::vector<float>* playOut, bool probe = false) {
  auto* h = reinterpret_cast<WHACableExchange*>(io.data());
  std::memset(h, 0, sizeof(*h));
  h->protocol = WHA_CABLE_PROTOCOL;
  h->cable = gCable - 1;
  h->frames = frames;
  h->hasRecord = record ? 1u : 0u;
  h->rate = probe ? 0 : gRate;
  h->channels = probe ? 0 : gChannels;
  h->format = probe ? 0 : gFormat;
  float* audio = reinterpret_cast<float*>(io.data() + sizeof(WHACableExchange));
  const DWORD bytes = static_cast<DWORD>(sizeof(WHACableExchange) + frames * gChannels * sizeof(float));
  if (record) std::memcpy(audio, record, frames * gChannels * sizeof(float));
  DWORD got = 0;
  if (!DeviceIoControl(device, IOCTL_WHA_CABLE_EXCHANGE, io.data(), record ? bytes : sizeof(WHACableExchange),
                       io.data(), bytes, &got, nullptr) || got != bytes)
    return false;
  std::memcpy(&reply, h, sizeof(reply));
  if (playOut)
    for (unsigned i = 0; i < frames; ++i) playOut->push_back(audio[gChannels * i]);  // Ch 1
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
    else if (!std::strcmp(argv[i], "--rate")) gRate = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--channels")) gChannels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--format")) gFormat = static_cast<unsigned>(std::atoi(argv[++i]));
  }
  if (block < 16 || block > WHA_CABLE_MAX_FRAMES) block = 256;
  if (gCable < 1 || gCable > 8) gCable = 1;
  if (gChannels != 2 && gChannels != 4 && gChannels != 6 && gChannels != 8) gChannels = 2;
  if (gFormat > 4) gFormat = 0;
  if (gRate < 8000 || gRate > 384000) gRate = 48000;
  std::printf("cable-live: cable %u, %.0f s, Worker block %u, format %u Hz %u ch format %u\n", gCable, seconds, block,
              gRate, gChannels, gFormat);

  HANDLE device = CreateFileW(WHA_CABLE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (device == INVALID_HANDLE_VALUE) {
    std::printf("  control device %ls: error %lu (driver not installed, or a Worker has it open)\n",
                WHA_CABLE_USER_PATH, GetLastError());
    check("control device opens", false);
    return 1;
  }
  std::vector<unsigned char> io(sizeof(WHACableExchange) + WHA_CABLE_MAX_FRAMES * WHA_CABLE_MAX_CHANNELS * sizeof(float));
  WHACableExchange reply{};
  const bool probed = Exchange(device, io, 0, nullptr, reply, nullptr, true);
  check("control device answers (protocol 2)", probed && reply.protocol == WHA_CABLE_PROTOCOL);
  if (!probed) return 1;
  std::printf("  driver: %u cable(s); cable %u offers %u Hz %u ch format %u\n", reply.cables, gCable, reply.rate,
              reply.channels, reply.format);

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

  // The format: send it (no audio yet); CableEndpointSync (as in the Master) sets both endpoints'
  // Windows device format to it, since Windows does not follow the driver's format change by itself.
  DeviceFormat before, renderNow, captureNow;
  ReadDeviceFormat(renderDevice, before);
  PrintFormat("playback device format before", before);
  const bool sent = Exchange(device, io, 0, nullptr, reply, nullptr);
  check("driver takes the format", sent && reply.rate == gRate && reply.channels == gChannels && reply.format == gFormat);
  wha::CableEndpointSync endpointSync([](int cable, uint32_t& r, uint32_t& ch, uint32_t& f) {
    if (cable != static_cast<int>(gCable) - 1) return false;
    r = gRate;
    ch = gChannels;
    f = gFormat;
    return true;
  });
  endpointSync.start();
  endpointSync.wake();
  LARGE_INTEGER freq, start, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  double switched = -1;
  for (;;) {
    QueryPerformanceCounter(&now);
    const double t = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
    if (ReadDeviceFormat(renderDevice, renderNow) && ReadDeviceFormat(captureDevice, captureNow) &&
        WantedFormat(renderNow) && WantedFormat(captureNow)) {
      switched = t;
      break;
    }
    if (t > 15) break;
    Sleep(100);
  }
  PrintFormat("playback device format now", renderNow);
  PrintFormat("recording device format now", captureNow);
  if (switched >= 0) std::printf("  endpoints switched in %.1f s (%u set)\n", switched, endpointSync.switches());
  if (endpointSync.lastError())
    std::printf("  SetDeviceFormat error 0x%08lX\n", static_cast<unsigned long>(endpointSync.lastError()));
  endpointSync.stop();
  check("both endpoints use the cable's format (within 15 s)", switched >= 0);

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
  const unsigned renderCh = renderFormat->nChannels, captureCh = captureFormat->nChannels;
  const bool mixOk = renderFormat->wBitsPerSample == 32 && captureFormat->wBitsPerSample == 32 &&
                     renderFormat->nSamplesPerSec == gRate && renderCh == gChannels && captureCh == gChannels;
  check("engine mix format: float at the cable's rate and channels", mixOk);
  if (!mixOk) return 1;
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
  std::vector<float> fromWindows, toWindows, recordBlock(gChannels * block);
  QueryPerformanceCounter(&start);
  long long exchanged = 0, played = 0, recordPhase = 0;
  int exchangeErrors = 0;
  unsigned seenPlayRate = 0, seenRecordRate = 0;
  double maxLateMs = 0;
  int lateTicks = 0;                     // ticks more than one block late (after the first second)
  WHACableExchange counters = reply;     // the driver's underrun / drop counters at the last tick
  std::vector<std::string> driverEvents;
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
          for (unsigned c = 0; c < renderCh; ++c)
            f[renderCh * i + c] = kAmplitude * static_cast<float>(std::sin(2 * kPi * kPlayHz * played / rate));
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
      for (UINT32 i = 0; i < n; ++i) toWindows.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : f[captureCh * i]);
      captureClient->ReleaseBuffer(n);
    }
    // "Worker": exchange blocks that are due.
    while (static_cast<double>(exchanged + block) <= t * rate) {
      // How late this tick is (a block is due once its last frame's time has passed).
      const double lateMs = (t * rate - static_cast<double>(exchanged + block)) * 1000.0 / rate;
      if (t > 1 && lateMs > maxLateMs) maxLateMs = lateMs;
      if (t > 1 && lateMs * rate / 1000.0 > block) ++lateTicks;
      for (unsigned i = 0; i < block; ++i, ++recordPhase)
        for (unsigned c = 0; c < gChannels; ++c)
          recordBlock[gChannels * i + c] = kAmplitude * static_cast<float>(std::sin(2 * kPi * kRecordHz * recordPhase / rate));
      if (!Exchange(device, io, block, recordBlock.data(), reply, &fromWindows)) ++exchangeErrors;
      if (reply.playRate) seenPlayRate = reply.playRate;
      if (reply.recordRate) seenRecordRate = reply.recordRate;
      // The driver's counters moving: when, and how late this tick was then.
      if (exchanged > 0 && (reply.playUnderruns != counters.playUnderruns || reply.playDrops != counters.playDrops ||
                            reply.recordUnderruns != counters.recordUnderruns || reply.recordDrops != counters.recordDrops) &&
          driverEvents.size() < 12) {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "    %.3f s (tick %.1f ms late, fill play %u record %u): play underruns +%u drops +%u, record underruns +%u drops +%u",
                      t, lateMs, reply.playFill, reply.recordFill, reply.playUnderruns - counters.playUnderruns,
                      reply.playDrops - counters.playDrops, reply.recordUnderruns - counters.recordUnderruns,
                      reply.recordDrops - counters.recordDrops);
        driverEvents.push_back(line);
      }
      counters = reply;
      exchanged += block;
    }
    Sleep(1);
  }
  timeEndPeriod(1);
  render->Stop();
  capture->Stop();

  std::printf("  driver saw: playback %u Hz, recording %u Hz; playback underruns %u drops %u, recording underruns %u drops %u\n",
              seenPlayRate, seenRecordRate, reply.playUnderruns, reply.playDrops, reply.recordUnderruns, reply.recordDrops);
  std::printf("  Worker ticks: latest %.1f ms late, %d more than one block (%.1f ms) late\n", maxLateMs, lateTicks,
              block * 1000.0 / gRate);
  if (!driverEvents.empty()) std::printf("  driver counters moved:\n");
  for (const std::string& e : driverEvents) std::printf("%s\n", e.c_str());
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
