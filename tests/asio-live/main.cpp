// asio-live — open the REGISTERED driver exactly like a DAW (HKLM\SOFTWARE\ASIO -> CLSID ->
// CoCreateInstance on an STA thread) and stream through it on the real machine: real Slot Table,
// real Worker, real hardware endpoint. No DAW needed.
//
// Checks what can be measured without ears: bufferSwitch cadence between callbacks, sample
// position, and that the HW output really holds the default render endpoint exclusively while
// streaming (a second client gets AUDCLNT_E_DEVICE_IN_USE) and releases it after stop; and, via the
// DLL's WHAGetMasterStats, that the hardware paced the Master Clock with no underrun or dropped block.
// Output 1 carries a quiet 1 kHz tone; whether it is audible is still for a human to confirm.
//
// usage: asio-live [--driver "WinHookAudio Master"] [--seconds 3] [--silent] [--hw-buffer frames]
//                  [--render "name"] [--capture "name"] [--loop]
//   --hw-buffer: HW period (GENERAL "Hardware (KS Exclusive)").
//   --render / --capture: HW endpoints by friendly-name substring (default: Windows defaults).
//     These three are set before the driver fills in its defaults, so only when no other process
//     holds the Slot Table.
//   --loop: output 1 carries deterministic noise; expects it back on input 1 through a physical or
//     virtual loopback (e.g. --render "CABLE Input" --capture "CABLE Output"), and checks the
//     round-trip delay against the reported latencies and the signal quality.

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlwapi.h>
#include <dbghelp.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "WHAAsio.h"
#include "WHAMasterStats.h"
#include "WHARegister.h"
#include "WHASharedMemory.h"
#include "WHASlotTable.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

const double kPi = 3.14159265358979323846;

// Deterministic noise at -20 dBFS peak: a function of the sample index, so any delay is unambiguous.
float Noise(uint64_t n) {
  uint64_t x = n * 0x9E3779B97F4A7C15ull;
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 29;
  return 0.1f * (static_cast<float>(x >> 40) / static_cast<float>(1ull << 23) - 1.0f);
}

// ---- host state touched by the driver's clock thread ----
IASIO* gAsio = nullptr;
std::vector<ASIOBufferInfo> gBufs;
long gInputs = 0, gOutputs = 0, gBlock = 0;
double gRate = 0;
bool gTone = true;
bool gLoop = false;
std::vector<float> gIn1;  // input 1, every frame (--loop)
std::atomic<long> gSwitches{0};
std::atomic<bool> gPositionsOk{true};
std::atomic<bool> gResetRequested{false};
uint64_t gSent = 0, gExpectedPos = 0;
// Rate from the callback at gSettle on: a HW output takes ~10 ms to start draining, which is
// start-up latency, not clock rate.
long gSettle = 0;
LARGE_INTEGER gFirstSwitch{}, gLastSwitch{};
std::vector<float> gInPeak;

void BufferSwitch(long index, ASIOBool) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (gSwitches.load() == gSettle) gFirstSwitch = now;
  gLastSwitch = now;
  ASIOSamples pos{};
  ASIOTimeStamp ts{};
  if (gAsio->getSamplePosition(&pos, &ts) != ASE_OK || FromAsio64(pos.hi, pos.lo) != gExpectedPos) gPositionsOk = false;
  gExpectedPos += static_cast<uint64_t>(gBlock);
  for (long i = 0; i < gInputs; ++i) {
    const float* in = static_cast<const float*>(gBufs[i].buffers[index]);
    for (long f = 0; f < gBlock; ++f) gInPeak[i] = (std::max)(gInPeak[i], std::abs(in[f]));
    if (i == 0 && gLoop && gIn1.size() + static_cast<size_t>(gBlock) <= gIn1.capacity()) gIn1.insert(gIn1.end(), in, in + gBlock);
  }
  for (long o = 0; o < gOutputs; ++o) {
    float* out = static_cast<float*>(gBufs[gInputs + o].buffers[index]);
    for (long f = 0; f < gBlock; ++f)
      out[f] = o != 0 || !gTone ? 0.0f
               : gLoop           ? Noise(gSent + static_cast<uint64_t>(f))
                                 : static_cast<float>(0.1 * std::sin(2 * kPi * 1000.0 * static_cast<double>(gSent + f) / gRate));
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

// Try to open the HW output's render endpoint (gRenderId, empty = default) in shared mode as a second
// client. S_OK: nobody holds it exclusively. AUDCLNT_E_DEVICE_IN_USE: someone (our HW output) does.
std::string gRenderId;
HRESULT ProbeRender() {
  IMMDeviceEnumerator* e = nullptr;
  IMMDevice* d = nullptr;
  IAudioClient* c = nullptr;
  WAVEFORMATEX* f = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  if (SUCCEEDED(hr) && gRenderId.empty()) hr = e->GetDefaultAudioEndpoint(eRender, eConsole, &d);
  else if (SUCCEEDED(hr)) {
    wchar_t id[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, gRenderId.c_str(), -1, id, 128);
    hr = e->GetDevice(id, &d);
  }
  if (SUCCEEDED(hr)) hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
  if (SUCCEEDED(hr)) hr = c->GetMixFormat(&f);
  if (SUCCEEDED(hr)) hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, f, nullptr);
  if (f) CoTaskMemFree(f);
  if (c) c->Release();
  if (d) d->Release();
  if (e) e->Release();
  return hr;
}

// First active endpoint of `flow` whose friendly name contains `name` (case-insensitive); UTF-8 ID.
std::string FindEndpoint(EDataFlow flow, const char* name) {
  std::string found;
  IMMDeviceEnumerator* e = nullptr;
  IMMDeviceCollection* all = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e))) return found;
  if (SUCCEEDED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all))) {
    UINT count = 0;
    all->GetCount(&count);
    for (UINT i = 0; i < count && found.empty(); ++i) {
      IMMDevice* d = nullptr;
      IPropertyStore* props = nullptr;
      if (FAILED(all->Item(i, &d))) continue;
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal) {
        char friendly[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, friendly, sizeof(friendly), nullptr, nullptr);
        if (StrStrIA(friendly, name)) {
          LPWSTR id = nullptr;
          char idText[128] = {};
          if (SUCCEEDED(d->GetId(&id))) WideCharToMultiByte(CP_UTF8, 0, id, -1, idText, sizeof(idText), nullptr, nullptr);
          CoTaskMemFree(id);
          found = idText;
        }
      }
      PropVariantClear(&v);
      if (props) props->Release();
      d->Release();
    }
    all->Release();
  }
  e->Release();
  return found;
}

// Any unhandled exception in the process (host or driver threads): full minidump to
// %TEMP%\winhookaudio-asio-live-<pid>.dmp, then the normal crash.
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* info) {
  wchar_t path[MAX_PATH];
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  swprintf_s(path, L"%swinhookaudio-asio-live-%lu.dmp", dir, GetCurrentProcessId());
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithThreadInfo), &mei, nullptr, nullptr);
    CloseHandle(file);
    fwprintf(stderr, L"CRASH: exception 0x%08lX, dump %s\n", info->ExceptionRecord->ExceptionCode, path);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::wstring Widen(const char* s) {
  std::wstring w;
  for (; *s; ++s) w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
  return w;
}

}  // namespace

int main(int argc, char** argv) {
  SetUnhandledExceptionFilter(WriteCrashDump);
  std::wstring driver = L"WinHookAudio Master";
  double seconds = 3.0;
  long hwBuffer = -1;
  const char* renderName = nullptr;
  const char* captureName = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--driver") && i + 1 < argc) driver = Widen(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--silent")) gTone = false;
    else if (!std::strcmp(argv[i], "--hw-buffer") && i + 1 < argc) hwBuffer = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--render") && i + 1 < argc) renderName = argv[++i];
    else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) captureName = argv[++i];
    else if (!std::strcmp(argv[i], "--loop")) gLoop = true;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // DAWs create ASIO drivers on an STA thread

  // Create the Slot Table before the driver does: it keeps our hwBuffer and fills in the rest
  // (its defaults apply while version == 0). Held open until exit.
  HANDLE tableMap = nullptr;
  if (hwBuffer >= 0 || renderName || captureName) {
    tableMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(shm::kSlotTableSize),
                                  shm::kSlotTableName + 7);
    const bool fresh = tableMap && GetLastError() != ERROR_ALREADY_EXISTS;
    auto* table = tableMap ? static_cast<WHASlotTable*>(MapViewOfFile(tableMap, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize)) : nullptr;
    check("Slot Table not held by another process (--hw-buffer/--render/--capture)", fresh && table);
    if (!fresh || !table) return 1;
    if (hwBuffer >= 0) {
      table->general.hwBuffer = static_cast<uint32_t>(hwBuffer);
      std::printf("HW buffer (period) requested: %ld frames\n", hwBuffer);
    }
    const struct { EDataFlow flow; const char* name; char* id; } picks[] = {
        {eRender, renderName, table->general.hwRenderId}, {eCapture, captureName, table->general.hwCaptureId}};
    for (const auto& pick : picks) {
      if (!pick.name) continue;
      const std::string id = FindEndpoint(pick.flow, pick.name);
      std::printf("%s \"%s\" -> %s\n", pick.flow == eRender ? "Render" : "Capture", pick.name, id.empty() ? "(not found)" : id.c_str());
      check("Endpoint found by name", !id.empty());
      if (id.empty()) return 1;
      TruncateCopy(pick.id, kEndpointIdLen, id.c_str());
      if (pick.flow == eRender) gRenderId = id;
    }
  }

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

  const HRESULT before = ProbeRender();
  std::printf("HW render endpoint before start: 0x%08lX\n", static_cast<unsigned long>(before));
  check("HW render endpoint free before start", before == S_OK);

  // 2. CoCreateInstance with the CLSID as IID (the ASIO convention).
  HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&gAsio));
  check("CoCreateInstance from registry", SUCCEEDED(hr) && gAsio);
  if (!gAsio) return 1;
  auto getStats = reinterpret_cast<WHAGetMasterStatsFn>(GetProcAddress(GetModuleHandleW(dllPath), "WHAGetMasterStats"));
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
  gSettle = gBlock > 0 ? static_cast<long>(0.5 * gRate / gBlock) : 0;
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
  std::printf("Latencies before start: in %ld / out %ld frames\n", inLatency, outLatency);

  gIn1.reserve(static_cast<size_t>((seconds + 1.0) * gRate));  // no allocation in the callback
  std::printf("Streaming %.1f s%s...\n", seconds,
              !gTone ? " (silence)" : gLoop ? " (noise, -20 dBFS, output 1 -> expected on input 1)" : " (1 kHz tone, -20 dBFS, output 1)");
  check("start", gAsio->start() == ASE_OK);
  Sleep(static_cast<DWORD>(seconds * 500));
  const HRESULT during = ProbeRender();
  std::printf("HW render endpoint while streaming: 0x%08lX%s\n", static_cast<unsigned long>(during),
              during == AUDCLNT_E_DEVICE_IN_USE ? " (AUDCLNT_E_DEVICE_IN_USE)" : "");
  check("HW output holds its render endpoint exclusively", during == AUDCLNT_E_DEVICE_IN_USE);
  Sleep(static_cast<DWORD>(seconds * 500));
  WHAMasterStats st{};
  const bool haveStats = getStats && getStats(&st) == 0;  // before stop: counters live with the instance
  long inStreaming = 0, outStreaming = 0;
  gAsio->getLatencies(&inStreaming, &outStreaming);
  std::printf("Latencies while streaming: in %ld / out %ld frames\n", inStreaming, outStreaming);
  LARGE_INTEGER stopFreq, stop0, stop1;
  QueryPerformanceFrequency(&stopFreq);
  QueryPerformanceCounter(&stop0);
  check("stop", gAsio->stop() == ASE_OK);
  QueryPerformanceCounter(&stop1);
  std::printf("stop() took %.0f ms\n", 1000.0 * static_cast<double>(stop1.QuadPart - stop0.QuadPart) / static_cast<double>(stopFreq.QuadPart));
  const long switches = gSwitches.load();
  Sleep(50);
  check("No bufferSwitch after stop", gSwitches.load() == switches);

  // Rate between the first and last callback: start/stop edges do not count.
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const double span = static_cast<double>(gLastSwitch.QuadPart - gFirstSwitch.QuadPart) / static_cast<double>(freq.QuadPart);
  const long counted = switches - 1 - gSettle;
  const double measured = counted > 0 ? static_cast<double>(counted) * gBlock / span : 0;
  std::printf("bufferSwitch: %ld total; after the first %ld: %.3f s = %.1f frames/s (%.3f%% off)\n", switches, gSettle, span,
              measured, 100.0 * (measured - gRate) / gRate);
  check("Master Clock rate within 0.1%", counted > 0 && std::abs(measured - gRate) < gRate * 0.001);
  check("WHAGetMasterStats exported and streaming", haveStats);
  if (haveStats) {
    std::printf("Stats: clock=%s ticks=%llu workerOverruns=%llu clockOverruns=%llu\n",
                st.clockSource == CLOCK_HARDWARE ? "hardware" : "internal", st.ticks, st.workerOverruns, st.clockOverruns);
    std::printf("       hwOpen=%d lastError=0x%08lX writes=%llu underruns=%llu drops=%llu fill %d..%d of %d frames\n",
                st.hwOpen, static_cast<unsigned long>(st.hwLastError), st.hwWrites, st.hwUnderruns, st.hwDrops,
                st.hwMinFill, st.hwMaxFill, st.hwCapacity);
    check("Master Clock paced by the HW output", st.clockSource == CLOCK_HARDWARE);
    check("HW output: no underrun, no dropped block", st.hwOpen && st.hwUnderruns == 0 && st.hwDrops == 0);
    // Measured: fill queued ahead of each new block + the device's stream latency.
    const long measuredOut = st.hwFillAtTick + st.hwStreamLatency;
    std::printf("Output latency measured: fill at tick %d + stream %d = %ld frames (%.1f ms)\n", st.hwFillAtTick,
                st.hwStreamLatency, measuredOut, 1000.0 * measuredOut / gRate);
    check("Reported output latency before start = while streaming", outLatency == outStreaming);
    check("Reported output latency within one block of measured", std::abs(outStreaming - measuredOut) <= gBlock);
    std::printf("HW input: open=%d lastError=0x%08lX reads=%llu starved=%llu trims=%llu glitches=%llu growths=%llu"
                " drift %+.1f ppm (%s)\n",
                st.hwInOpen, static_cast<unsigned long>(st.hwInLastError), st.hwInReads, st.hwInStarved, st.hwInTrims,
                st.hwInGlitches, st.hwInGrowths, st.hwInDriftPpmMilli / 1000.0, st.hwInDriftEngaged ? "resampling" : "bit-exact");
    if (st.hwInOpen) {
      // Measured: backlog at each read (age of the frame read, from capture timestamps) + resampler +
      // the tick the block waits in its IN slot.
      const long measuredIn = st.hwInMeanFill + 9 + gBlock;
      std::printf("Input latency measured: backlog at read %d (target %d) + resampler 9 + slot %ld = %ld frames (%.1f ms)\n",
                  st.hwInMeanFill, st.hwInTarget, gBlock, measuredIn, 1000.0 * measuredIn / gRate);
      if (st.hwInGrowths == 0) check("Reported input latency before start = while streaming", inLatency == inStreaming);
      check("Reported input latency within one block of measured", std::abs(inStreaming - measuredIn) <= gBlock);
      check("HW input: no trim; starves only while the target grew", st.hwInTrims == 0 && st.hwInStarved == st.hwInGrowths);
    }
  }
  check("getSamplePosition = frames before each bufferSwitch", gPositionsOk.load());
  for (long i = 0; i < gInputs; ++i) std::printf("Input %ld peak: %.4f\n", i + 1, gInPeak[i]);

  // --loop: find the delay d where input1[n] best matches Noise(n - d), over the last second.
  long loopDelay = -1;
  double loopSnr = 0;
  if (gLoop) {
    const long n = static_cast<long>(gIn1.size());
    const long window = static_cast<long>(gRate);
    const long maxDelay = static_cast<long>(0.5 * gRate);
    if (n > window + maxDelay) {
      double best = 0;
      for (long d = 0; d <= maxDelay; ++d) {
        double c = 0;
        for (long i = n - window; i < n; ++i) c += static_cast<double>(gIn1[i]) * Noise(static_cast<uint64_t>(i - d));
        if (c > best) { best = c; loopDelay = d; }
      }
      double sig = 0, err = 0;
      for (long i = n - window; i < n && loopDelay >= 0; ++i) {
        const double ref = Noise(static_cast<uint64_t>(i - loopDelay));
        sig += ref * ref;
        err += (gIn1[i] - ref) * (gIn1[i] - ref);
      }
      loopSnr = err > 0 ? 10.0 * std::log10(sig / err) : 200.0;
    }
    // Delay per 250 ms window: a stable path shows one value; jumps show where the stream slipped.
    long firstDelay = -2;
    bool steady = true;
    std::printf("Loop delay per 250 ms:");
    const long step = static_cast<long>(gRate / 4);
    for (long w = 0; w + step <= n; w += step) {
      long bestD = -1;
      double bestC = 0, energy = 0;
      for (long i = w; i < w + step; ++i) energy += static_cast<double>(gIn1[i]) * gIn1[i];
      for (long d = 0; d <= maxDelay && d <= w + step && energy > 0; ++d) {
        double c = 0;
        for (long i = (std::max)(w, d); i < w + step; ++i) c += static_cast<double>(gIn1[i]) * Noise(static_cast<uint64_t>(i - d));
        if (c > bestC) { bestC = c; bestD = d; }
      }
      if (bestD < 0) std::printf(" -"); else std::printf(" %ld", bestD);
      if (w >= static_cast<long>(gSettle) * gBlock) {  // after the settle, every window must agree
        if (firstDelay == -2) firstDelay = bestD;
        steady = steady && bestD >= 0 && bestD == firstDelay;
      }
    }
    std::printf("\n");
    const long reported = outStreaming + inStreaming;
    std::printf("Loop: round trip %ld frames (%.2f ms) = reported out %ld + in %ld + %ld outside the driver; SNR %.1f dB\n",
                loopDelay, 1000.0 * loopDelay / gRate, outStreaming, inStreaming, loopDelay - reported, loopSnr);
    std::printf("      (a virtual cable adds its own unreported, run-dependent delay: VB-Cable measured 42-55 ms alone)\n");
    check("Loop: output 1 comes back on input 1", loopDelay >= 0);
    check("Loop: SNR >= 60 dB (bit-exact apart from 16-bit quantization)", loopSnr >= 60.0);
    check("Loop: delay constant after settle (no slipped or repeated frames)", steady && firstDelay >= 0);
    check("Loop: round trip not shorter than the reported latencies", loopDelay >= reported - gBlock);
  }

  gAsio->disposeBuffers();
  gAsio->Release();
  gAsio = nullptr;
  const HRESULT after = ProbeRender();
  check("HW render endpoint released after stop", after == S_OK);
  CoUninitialize();
  if (tableMap) CloseHandle(tableMap);

  std::printf("{\"schema_version\":1,\"operation\":\"asio_live\",\"switches\":%ld,\"rate\":%.1f,\"hw_exclusive_held\":%s,"
              "\"hw_clock\":%s,\"hw_underruns\":%llu,\"loop_delay\":%ld,\"loop_snr\":%.1f,\"audible\":\"unverified\",\"pass\":%s}\n",
              switches, measured, during == AUDCLNT_E_DEVICE_IN_USE ? "true" : "false",
              haveStats && st.clockSource == CLOCK_HARDWARE ? "true" : "false", st.hwUnderruns, loopDelay, loopSnr, gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
