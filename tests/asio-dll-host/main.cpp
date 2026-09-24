// asio-dll-host — load the built Master + Bridge driver DLLs exactly like an ASIO host
// (DllGetClassObject -> IClassFactory::CreateInstance(CLSID as IID) -> IASIO vtable from the SDK
// headers) and stream through them: Master Clock bufferSwitch, Worker loopback, Shared Bridge both ways.
// Offline: no DAW, no device, no registry. stream_verified stays false.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "WHAAsio.h"
#include "WHAMasterStats.h"
#include "WHARegister.h"
#include "WHASharedMemory.h"
#include "WHASlotTable.h"
#include "virtual/WHACableProtocol.h"

using namespace wha;

namespace {

bool gPass = true;
void check(const char* name, bool ok) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) gPass = false;
}

constexpr long kBlock = 128;
constexpr double kRate = 48000.0;
const double kPi = 3.14159265358979323846;

struct Loaded {
  HMODULE dll = nullptr;
  IASIO* asio = nullptr;
};

Loaded LoadDriver(const char* path, REFCLSID clsid) {
  Loaded l;
  l.dll = LoadLibraryA(path);
  if (!l.dll) return l;
  using GetClassObject = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
  auto get = reinterpret_cast<GetClassObject>(GetProcAddress(l.dll, "DllGetClassObject"));
  IClassFactory* factory = nullptr;
  if (get && SUCCEEDED(get(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) && factory) {
    factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&l.asio));  // ASIO hosts pass the CLSID as IID
    factory->Release();
  }
  return l;
}

// ---- Master host state (callbacks are plain functions, as in a real host) ----
IASIO* gMaster = nullptr;
ASIOBufferInfo gMasterBufs[4]{};  // in0, in1, out0, out1
std::vector<float> gIn0, gIn1;
std::atomic<long> gMasterSwitches{0};
uint64_t gSent = 0;
bool gPositionsOk = true;
uint64_t gExpectedPos = 0;
LARGE_INTEGER gFirstSwitch{}, gLastSwitch{};  // rate between callbacks: start/stop edges do not count

float Sine(uint64_t n) { return static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * static_cast<double>(n) / kRate)); }

void MasterSwitch(long index, ASIOBool) {
  QueryPerformanceCounter(&gLastSwitch);
  if (gMasterSwitches.load() == 0) gFirstSwitch = gLastSwitch;
  ASIOSamples pos{};
  ASIOTimeStamp ts{};
  if (gMaster->getSamplePosition(&pos, &ts) != ASE_OK || FromAsio64(pos.hi, pos.lo) != gExpectedPos) gPositionsOk = false;
  gExpectedPos += kBlock;
  const float* in0 = static_cast<const float*>(gMasterBufs[0].buffers[index]);
  const float* in1 = static_cast<const float*>(gMasterBufs[1].buffers[index]);
  gIn0.insert(gIn0.end(), in0, in0 + kBlock);
  gIn1.insert(gIn1.end(), in1, in1 + kBlock);
  float* out0 = static_cast<float*>(gMasterBufs[2].buffers[index]);
  float* out1 = static_cast<float*>(gMasterBufs[3].buffers[index]);
  for (long f = 0; f < kBlock; ++f) {
    out0[f] = Sine(gSent + static_cast<uint64_t>(f));  // -> VIRTUAL loopback -> IN0
    out1[f] = 0.5f;                                    // -> BRIDGE1 broadcast -> Slave DAW input
  }
  gSent += kBlock;
  gMasterSwitches.fetch_add(1);
}
long MasterMessage(long selector, long value, void*, double*) {
  if (selector == kAsioSelectorSupported) return value == kAsioResetRequest ? 1 : 0;
  return 0;
}

// ---- Slave DAW on the Bridge ----
ASIOBufferInfo gBridgeBufs[2]{};  // in0, out0
std::vector<float> gBridgeIn;
std::atomic<long> gBridgeSwitches{0};
void BridgeSwitch(long index, ASIOBool) {
  const float* in = static_cast<const float*>(gBridgeBufs[0].buffers[index]);
  gBridgeIn.insert(gBridgeIn.end(), in, in + kBlock);
  float* out = static_cast<float*>(gBridgeBufs[1].buffers[index]);
  for (long f = 0; f < kBlock; ++f) out[f] = 0.25f;  // -> summed into Master IN1 (BRIDGE1)
  gBridgeSwitches.fetch_add(1);
}
long BridgeMessage(long, long, void*, double*) { return 0; }

std::wstring ReadHklmString(const std::wstring& key, const wchar_t* value) {
  wchar_t buf[MAX_PATH] = {};
  DWORD bytes = sizeof(buf);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return L"";
  return buf;
}
bool HklmKeyExists(const std::wstring& key) {
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  RegCloseKey(k);
  return true;
}

// regsvr32 path: DllRegisterServer/DllUnregisterServer with HKLM redirected to a scratch HKCU key,
// so it runs without elevation and never touches the real registry.
void RegistrationChecks(const char* label, HMODULE dll, const AsioRegistration* regs, int count) {
  const wchar_t* sandboxPath = L"Software\\WinHookAudioRegTest";
  RegDeleteTreeW(HKEY_CURRENT_USER, sandboxPath);
  HKEY sandbox = nullptr;
  RegCreateKeyExW(HKEY_CURRENT_USER, sandboxPath, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr);
  const bool redirected = sandbox && RegOverridePredefKey(HKEY_LOCAL_MACHINE, sandbox) == ERROR_SUCCESS;
  char name[96];
  std::snprintf(name, sizeof(name), "%s: HKLM redirected to HKCU sandbox", label);
  check(name, redirected);
  if (!redirected) return;
  using ServerFn = HRESULT(__stdcall*)();
  auto reg = reinterpret_cast<ServerFn>(GetProcAddress(dll, "DllRegisterServer"));
  auto unreg = reinterpret_cast<ServerFn>(GetProcAddress(dll, "DllUnregisterServer"));
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(dll, modulePath, MAX_PATH);

  std::snprintf(name, sizeof(name), "%s: DllRegisterServer S_OK", label);
  check(name, reg && reg() == S_OK);
  bool ok = true;
  for (int i = 0; i < count; ++i) {
    const std::wstring clsidKey = ClsidKey(*regs[i].clsid);
    ok = ok && _wcsicmp(ReadHklmString(clsidKey + L"\\InprocServer32", nullptr).c_str(), modulePath) == 0;
    ok = ok && ReadHklmString(clsidKey + L"\\InprocServer32", L"ThreadingModel") == L"Apartment";
    ok = ok && ReadHklmString(AsioKey(regs[i].name), L"CLSID") == GuidString(*regs[i].clsid);
    ok = ok && !ReadHklmString(AsioKey(regs[i].name), L"Description").empty();
  }
  std::snprintf(name, sizeof(name), "%s: CLSID InprocServer32 = this DLL, Apartment, SOFTWARE\\ASIO entries", label);
  check(name, ok);
  std::snprintf(name, sizeof(name), "%s: DllUnregisterServer removes every key", label);
  bool gone = unreg && unreg() == S_OK;
  for (int i = 0; i < count; ++i) gone = gone && !HklmKeyExists(ClsidKey(*regs[i].clsid)) && !HklmKeyExists(AsioKey(regs[i].name));
  check(name, gone);

  RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
  RegCloseKey(sandbox);
  RegDeleteTreeW(HKEY_CURRENT_USER, sandboxPath);
}

WHASlot Slot(WHASlotType type, int32_t src, bool loopback, const char* name) {
  WHASlot s{};
  s.type = type;
  s.srcChannel = src;
  s.enabled = 1;
  s.loopback = loopback ? 1 : 0;
  SetSlotName(s, name);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: asio-dll-host <WinHookAudioMasterASIO64.dll> <WinHookAudioBridgeASIO64.dll>\n");
    return 2;
  }
#if WHA_HAVE_ASIO_SDK
  std::printf("ASIO headers: Steinberg SDK 2.3.4\n");
#else
  std::printf("ASIO headers: offline mirror\n");
#endif

  // Offline: hold the Virtual Cable driver's control device (exclusive, if installed) so the Worker
  // uses its in-process cable loop, as on a machine without the driver.
  HANDLE cableDriver = CreateFileW(WHA_CABLE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

  // The routing a user would set in the Control Panel: IN0/OUT0 VIRTUAL loopback, IN1/OUT1 BRIDGE1.
  HANDLE tableMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(shm::kSlotTableSize), shm::kSlotTableName + 7);
  auto* table = static_cast<WHASlotTable*>(MapViewOfFile(tableMap, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize));
  *table = WHASlotTable{};
  table->masterInCount = 2;
  table->masterIn[0] = Slot(SLOT_VIRTUAL, 0, false, "Loop In");
  table->masterIn[1] = Slot(SLOT_BRIDGE1, 0, false, "From Slave DAW");
  table->masterOutCount = 2;
  table->masterOut[0] = Slot(SLOT_VIRTUAL, 0, true, "Loop Out");
  table->masterOut[1] = Slot(SLOT_BRIDGE1, 0, false, "To Slave DAW");
  table->general.sampleRate = static_cast<uint32_t>(kRate);
  table->general.asioBuffer = kBlock;
  table->version = 1;

  Loaded master = LoadDriver(argv[1], CLSID_WinHookMaster);
  check("Master DLL exports DllGetClassObject -> IASIO", master.asio != nullptr);
  if (!master.asio) return 1;
  gMaster = master.asio;
  const AsioRegistration masterReg[] = {{&CLSID_WinHookMaster, L"WinHookAudio Master", L"WinHookAudio Master (512)"}};
  RegistrationChecks("Master", master.dll, masterReg, 1);
  IASIO* m = master.asio;
  check("Master init -> ASIOTrue", m->init(GetDesktopWindow()) == ASIOTrue);
  char name[32] = {};
  m->getDriverName(name);
  check("Master getDriverName", std::strcmp(name, "WinHookAudio Master") == 0);
  long nIn = 0, nOut = 0;
  check("Master getChannels 2/2", m->getChannels(&nIn, &nOut) == ASE_OK && nIn == 2 && nOut == 2);
  ASIOChannelInfo ci{};
  ci.channel = 1;
  ci.isInput = ASIOTrue;
  check("Master channel names from Slot Table", m->getChannelInfo(&ci) == ASE_OK && std::strcmp(ci.name, "From Slave DAW") == 0);
  long mn = 0, mx = 0, pref = 0, gran = 0;
  check("Master buffer = Master Clock 128", m->getBufferSize(&mn, &mx, &pref, &gran) == ASE_OK && pref == kBlock);
  ASIOSampleRate sr = 0;
  check("Master rate 48000", m->getSampleRate(&sr) == ASE_OK && sr == kRate && m->canSampleRate(kRate) == ASE_OK);

  ASIOCallbacks mcb{};
  mcb.bufferSwitch = MasterSwitch;
  mcb.asioMessage = MasterMessage;
  gMasterBufs[0] = {ASIOTrue, 0, {}};
  gMasterBufs[1] = {ASIOTrue, 1, {}};
  gMasterBufs[2] = {ASIOFalse, 0, {}};
  gMasterBufs[3] = {ASIOFalse, 1, {}};
  check("Master createBuffers", m->createBuffers(gMasterBufs, 4, kBlock, &mcb) == ASE_OK);

  Loaded bridge = LoadDriver(argv[2], CLSID_WinHookBridge1);
  check("Bridge DLL exports DllGetClassObject -> IASIO", bridge.asio != nullptr);
  IASIO* b = bridge.asio;
  if (bridge.dll) {
    const AsioRegistration bridgeRegs[] = {{&CLSID_WinHookBridge1, L"WinHookAudio Bridge 1", L"x"},
                                           {&CLSID_WinHookBridge2, L"WinHookAudio Bridge 2", L"x"},
                                           {&CLSID_WinHookBridge3, L"WinHookAudio Bridge 3", L"x"},
                                           {&CLSID_WinHookBridge4, L"WinHookAudio Bridge 4", L"x"}};
    RegistrationChecks("Bridge", bridge.dll, bridgeRegs, 4);
  }
  if (b) {
    check("Bridge init -> ASIOTrue", b->init(GetDesktopWindow()) == ASIOTrue);
    long bIn = 0, bOut = 0;
    check("Bridge channels: in = Master OUT BRIDGE1, out = Master IN BRIDGE1",
          b->getChannels(&bIn, &bOut) == ASE_OK && bIn == 1 && bOut == 1);
    ASIOChannelInfo bci{};
    bci.isInput = ASIOTrue;
    check("Bridge input named after Master OUT slot", b->getChannelInfo(&bci) == ASE_OK && std::strcmp(bci.name, "To Slave DAW") == 0);
    ASIOCallbacks bcb{};
    bcb.bufferSwitch = BridgeSwitch;
    bcb.asioMessage = BridgeMessage;
    gBridgeBufs[0] = {ASIOTrue, 0, {}};
    gBridgeBufs[1] = {ASIOFalse, 0, {}};
    check("Bridge createBuffers", b->createBuffers(gBridgeBufs, 2, kBlock, &bcb) == ASE_OK);
    check("Bridge start", b->start() == ASE_OK);
  }

  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  check("Master start", m->start() == ASE_OK);
  Sleep(600);
  // A Control Panel Save mid-stream (TableChanged, same table): the loopback must stay sample-exact.
  HANDLE changed = OpenEventA(EVENT_MODIFY_STATE, FALSE, shm::kTableChangedName + 7);
  check("Save mid-stream signals TableChanged", changed && SetEvent(changed));
  if (changed) CloseHandle(changed);
  Sleep(400);
  WHAMasterStats stats{};
  auto getStats = reinterpret_cast<WHAGetMasterStatsFn>(GetProcAddress(master.dll, "WHAGetMasterStats"));
  check("WHAGetMasterStats while streaming", getStats && getStats(&stats) == 0);
  check("Master stop", m->stop() == ASE_OK);
  QueryPerformanceCounter(&t1);
  if (b) b->stop();
  const long switches = gMasterSwitches.load();
  Sleep(50);
  check("No bufferSwitch after stop", gMasterSwitches.load() == switches);

  const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
  const double span = static_cast<double>(gLastSwitch.QuadPart - gFirstSwitch.QuadPart) / static_cast<double>(f.QuadPart);
  const double measuredRate = switches > 1 ? (switches - 1) * kBlock / span : 0;
  std::printf("Master Clock: %ld bufferSwitch in %.3f s (%.3f s between callbacks) = %.0f frames/s; Bridge: %ld\n",
              switches, seconds, span, measuredRate, gBridgeSwitches.load());
  std::printf("Master Clock source %s, clockOverruns %llu, workerOverruns %llu\n",
              stats.clockSource == CLOCK_HARDWARE ? "hardware" : "internal", stats.clockOverruns, stats.workerOverruns);
  check("Master Clock on the internal timeline (no HW slot)", stats.clockSource == CLOCK_INTERNAL);
  check("Master Clock rate within 1% of 48000", std::abs(measuredRate - kRate) < kRate * 0.01);
  check("getSamplePosition = frames before each bufferSwitch", gPositionsOk);

  // Loopback: IN0 must be the sine the DAW wrote to OUT0, delayed by a whole number of buffers.
  long delay = -1;
  for (long d = 0; d <= 4 * kBlock && delay < 0; d += kBlock) {
    bool match = gIn0.size() > static_cast<size_t>(d + 4 * kBlock);
    for (size_t n = gIn0.size() / 2; match && n < gIn0.size(); ++n) match = std::abs(gIn0[n] - Sine(n - d)) < 1e-6f;
    if (match) delay = d;
  }
  std::printf("Loopback OUT0 -> IN0 delay: %ld frames\n", delay);
  if (delay < 0) {  // per-block delay (in blocks), '?' = no whole-block match: shows where the stream slipped
    std::printf("Loopback per-block delay:");
    for (size_t blk = 4; blk < gIn0.size() / kBlock; ++blk) {
      int found = -1;
      for (int d = 0; d <= 4 && found < 0; ++d) {
        bool match = true;
        for (size_t i = 0; match && i < static_cast<size_t>(kBlock); ++i) {
          const size_t n = blk * kBlock + i;
          match = std::abs(gIn0[n] - Sine(n - static_cast<size_t>(d) * kBlock)) < 1e-6f;
        }
        if (match) found = d;
      }
      if (found < 0) std::printf(" [%zu]?", blk); else std::printf(" %d", found);
    }
    std::printf("\n");
  }
  check("VIRTUAL loopback OUT0 -> Worker -> IN0 sample-exact", delay >= 0);

  if (b) {
    const float expectedMix = std::tanh(0.25f);
    size_t goodMix = 0, goodBcast = 0;
    for (size_t n = gIn1.size() / 2; n < gIn1.size(); ++n) goodMix += std::abs(gIn1[n] - expectedMix) < 1e-5f;
    for (size_t n = gBridgeIn.size() / 2; n < gBridgeIn.size(); ++n) goodBcast += std::abs(gBridgeIn[n] - 0.5f) < 1e-6f;
    const double mixShare = gIn1.empty() ? 0 : goodMix / (gIn1.size() / 2.0);
    const double bcastShare = gBridgeIn.empty() ? 0 : goodBcast / (gBridgeIn.size() / 2.0);
    std::printf("Bridge: Master IN1 = tanh(0.25) on %.1f%% of samples, Slave input = 0.5 on %.1f%%\n", mixShare * 100,
                bcastShare * 100);
    check("Bridge follows Master Clock", std::labs(gBridgeSwitches.load() - switches) <= switches / 10 + 2);
    check("Slave DAW output summed into Master IN (BRIDGE1)", mixShare > 0.95);
    check("Master OUT (BRIDGE1) broadcast to Slave DAW input", bcastShare > 0.95);
    b->disposeBuffers();
    b->Release();
  }
  check("Master disposeBuffers", m->disposeBuffers() == ASE_OK);
  m->Release();
  if (bridge.dll) FreeLibrary(bridge.dll);
  FreeLibrary(master.dll);
  UnmapViewOfFile(table);
  CloseHandle(tableMap);
  if (cableDriver != INVALID_HANDLE_VALUE) CloseHandle(cableDriver);

  std::printf("{\"schema_version\":1,\"operation\":\"asio_dll_host\",\"stream_verified\":false,\"pass\":%s}\n",
              gPass ? "true" : "false");
  return gPass ? 0 : 1;
}
