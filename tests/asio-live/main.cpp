// asio-live — open the REGISTERED driver exactly like a DAW (HKLM\SOFTWARE\ASIO -> CLSID ->
// CoCreateInstance on an STA thread) and stream through it on the real machine: real Slot Table,
// real Worker, real hardware endpoint. No DAW needed.
//
// Checks what can be measured without ears: bufferSwitch cadence between callbacks, sample
// position, and that the HW output really holds the default render endpoint exclusively while
// streaming (a second client gets AUDCLNT_E_DEVICE_IN_USE) and releases it after stop.
// Output 1 carries a quiet 1 kHz tone; whether it is audible is still for a human to confirm.
//
// usage: asio-live [--driver "WinHookAudio Master"] [--seconds 3] [--silent]

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "WHAAsio.h"
#include "WHARegister.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

const double kPi = 3.14159265358979323846;

// ---- host state touched by the driver's clock thread ----
IASIO* gAsio = nullptr;
std::vector<ASIOBufferInfo> gBufs;
long gInputs = 0, gOutputs = 0, gBlock = 0;
double gRate = 0;
bool gTone = true;
std::atomic<long> gSwitches{0};
std::atomic<bool> gPositionsOk{true};
std::atomic<bool> gResetRequested{false};
uint64_t gSent = 0, gExpectedPos = 0;
LARGE_INTEGER gFirstSwitch{}, gLastSwitch{};
std::vector<float> gInPeak;

void BufferSwitch(long index, ASIOBool) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (gSwitches.load() == 0) gFirstSwitch = now;
  gLastSwitch = now;
  ASIOSamples pos{};
  ASIOTimeStamp ts{};
  if (gAsio->getSamplePosition(&pos, &ts) != ASE_OK || FromAsio64(pos.hi, pos.lo) != gExpectedPos) gPositionsOk = false;
  gExpectedPos += static_cast<uint64_t>(gBlock);
  for (long i = 0; i < gInputs; ++i) {
    const float* in = static_cast<const float*>(gBufs[i].buffers[index]);
    for (long f = 0; f < gBlock; ++f) gInPeak[i] = (std::max)(gInPeak[i], std::abs(in[f]));
  }
  for (long o = 0; o < gOutputs; ++o) {
    float* out = static_cast<float*>(gBufs[gInputs + o].buffers[index]);
    for (long f = 0; f < gBlock; ++f)
      out[f] = (o == 0 && gTone) ? static_cast<float>(0.1 * std::sin(2 * kPi * 1000.0 * static_cast<double>(gSent + f) / gRate)) : 0.0f;
  }
  gSent += static_cast<uint64_t>(gBlock);
  gSwitches.fetch_add(1);
}
ASIOTime* BufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) {
  BufferSwitch(index, direct);
  return params;
}
void SampleRateDidChange(ASIOSampleRate) {}
long AsioMessage(long selector, long value, void*, double*) {
  switch (selector) {
    case kAsioSelectorSupported: return value == kAsioResetRequest || value == kAsioEngineVersion ? 1 : 0;
    case kAsioEngineVersion: return 2;
    case kAsioResetRequest: gResetRequested = true; return 1;
    default: return 0;
  }
}

// Try to open the default render endpoint in shared mode as a second client.
// S_OK: nobody holds it exclusively. AUDCLNT_E_DEVICE_IN_USE: someone (our HW output) does.
HRESULT ProbeDefaultRender() {
  IMMDeviceEnumerator* e = nullptr;
  IMMDevice* d = nullptr;
  IAudioClient* c = nullptr;
  WAVEFORMATEX* f = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  if (SUCCEEDED(hr)) hr = e->GetDefaultAudioEndpoint(eRender, eConsole, &d);
  if (SUCCEEDED(hr)) hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
  if (SUCCEEDED(hr)) hr = c->GetMixFormat(&f);
  if (SUCCEEDED(hr)) hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, f, nullptr);
  if (f) CoTaskMemFree(f);
  if (c) c->Release();
  if (d) d->Release();
  if (e) e->Release();
  return hr;
}

std::wstring Widen(const char* s) {
  std::wstring w;
  for (; *s; ++s) w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
  return w;
}

}  // namespace

int main(int argc, char** argv) {
  std::wstring driver = L"WinHookAudio Master";
  double seconds = 3.0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--driver") && i + 1 < argc) driver = Widen(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--silent")) gTone = false;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // DAWs create ASIO drivers on an STA thread

  // 1. Registry discovery, as a DAW does it.
  wchar_t clsidText[64] = {};
  DWORD bytes = sizeof(clsidText);
  const bool registered = RegGetValueW(HKEY_LOCAL_MACHINE, AsioKey(driver.c_str()).c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr,
                                       clsidText, &bytes) == ERROR_SUCCESS;
  check("Registered under HKLM\\SOFTWARE\\ASIO", registered);
  if (!registered) return 1;
  CLSID clsid{};
  CLSIDFromString(clsidText, &clsid);
  wchar_t dllPath[MAX_PATH] = {};
  bytes = sizeof(dllPath);
  RegGetValueW(HKEY_LOCAL_MACHINE, (ClsidKey(clsid) + L"\\InprocServer32").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, dllPath, &bytes);
  std::printf("Driver DLL: %ls\n", dllPath);

  const HRESULT before = ProbeDefaultRender();
  std::printf("Default render endpoint before start: 0x%08lX\n", static_cast<unsigned long>(before));
  check("Default render endpoint free before start", before == S_OK);

  // 2. CoCreateInstance with the CLSID as IID (the ASIO convention).
  HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&gAsio));
  check("CoCreateInstance from registry", SUCCEEDED(hr) && gAsio);
  if (!gAsio) return 1;
  char text[128] = {};
  const bool inited = gAsio->init(GetConsoleWindow()) == ASIOTrue;
  check("init -> ASIOTrue", inited);
  if (!inited) {
    gAsio->getErrorMessage(text);
    std::printf("init error: %s\n", text);
    return 1;
  }
  gAsio->getDriverName(text);
  std::printf("Driver name: %s, version %ld\n", text, gAsio->getDriverVersion());

  long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
  gAsio->getChannels(&gInputs, &gOutputs);
  gAsio->getBufferSize(&minSize, &maxSize, &preferred, &granularity);
  gAsio->getSampleRate(&gRate);
  gBlock = preferred;
  std::printf("Channels: %ld in / %ld out, buffer %ld, rate %.0f\n", gInputs, gOutputs, gBlock, gRate);
  check("Channels, buffer and rate reported", gInputs > 0 && gOutputs > 0 && gBlock > 0 && gRate > 0);
  for (long i = 0; i < gInputs + gOutputs; ++i) {
    ASIOChannelInfo ci{};
    ci.channel = i < gInputs ? i : i - gInputs;
    ci.isInput = i < gInputs ? ASIOTrue : ASIOFalse;
    gAsio->getChannelInfo(&ci);
    std::printf("  %s %ld: %s (type %ld)\n", ci.isInput ? "IN " : "OUT", ci.channel + 1, ci.name, static_cast<long>(ci.type));
  }

  // 3. Buffers for every channel, then stream.
  gBufs.assign(static_cast<size_t>(gInputs + gOutputs), ASIOBufferInfo{});
  for (long i = 0; i < gInputs + gOutputs; ++i) {
    gBufs[i].isInput = i < gInputs ? ASIOTrue : ASIOFalse;
    gBufs[i].channelNum = i < gInputs ? i : i - gInputs;
  }
  gInPeak.assign(static_cast<size_t>(gInputs), 0.0f);
  ASIOCallbacks cb{};
  cb.bufferSwitch = BufferSwitch;
  cb.sampleRateDidChange = SampleRateDidChange;
  cb.asioMessage = AsioMessage;
  cb.bufferSwitchTimeInfo = BufferSwitchTimeInfo;
  check("createBuffers", gAsio->createBuffers(gBufs.data(), gInputs + gOutputs, gBlock, &cb) == ASE_OK);
  long inLatency = 0, outLatency = 0;
  gAsio->getLatencies(&inLatency, &outLatency);
  std::printf("Latencies: in %ld / out %ld frames\n", inLatency, outLatency);

  std::printf("Streaming %.1f s%s...\n", seconds, gTone ? " (1 kHz tone, -20 dBFS, output 1)" : " (silence)");
  check("start", gAsio->start() == ASE_OK);
  Sleep(static_cast<DWORD>(seconds * 500));
  const HRESULT during = ProbeDefaultRender();
  std::printf("Default render endpoint while streaming: 0x%08lX%s\n", static_cast<unsigned long>(during),
              during == AUDCLNT_E_DEVICE_IN_USE ? " (AUDCLNT_E_DEVICE_IN_USE)" : "");
  check("HW output holds default render endpoint exclusively", during == AUDCLNT_E_DEVICE_IN_USE);
  Sleep(static_cast<DWORD>(seconds * 500));
  check("stop", gAsio->stop() == ASE_OK);
  const long switches = gSwitches.load();
  Sleep(50);
  check("No bufferSwitch after stop", gSwitches.load() == switches);

  // Rate between the first and last callback: start/stop edges do not count.
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const double span = static_cast<double>(gLastSwitch.QuadPart - gFirstSwitch.QuadPart) / static_cast<double>(freq.QuadPart);
  const double measured = switches > 1 ? static_cast<double>(switches - 1) * gBlock / span : 0;
  std::printf("bufferSwitch: %ld in %.3f s = %.1f frames/s (%.3f%% off)\n", switches, span, measured, 100.0 * (measured - gRate) / gRate);
  check("Master Clock rate within 0.5%", switches > 1 && std::abs(measured - gRate) < gRate * 0.005);
  check("getSamplePosition = frames before each bufferSwitch", gPositionsOk.load());
  for (long i = 0; i < gInputs; ++i) std::printf("Input %ld peak: %.4f\n", i + 1, gInPeak[i]);

  gAsio->disposeBuffers();
  gAsio->Release();
  gAsio = nullptr;
  const HRESULT after = ProbeDefaultRender();
  check("Default render endpoint released after stop", after == S_OK);
  CoUninitialize();

  std::printf("{\"schema_version\":1,\"operation\":\"asio_live\",\"switches\":%ld,\"rate\":%.1f,\"hw_exclusive_held\":%s,"
              "\"audible\":\"unverified\",\"pass\":%s}\n",
              switches, measured, during == AUDCLNT_E_DEVICE_IN_USE ? "true" : "false", gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
