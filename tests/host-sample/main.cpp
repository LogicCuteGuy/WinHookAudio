// host-sample — the drivers' IASIO driven in-process the way an ASIO host would (ASIO SDK 2.3.4 ABI).
// No DAW, no device: stream_verified stays false.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include "WHASharedMemory.h"
#include "WHASlotsJson.h"
#include "WinHookMasterASIO.h"
#include "WinHookBridgeASIO.h"

using namespace wha;

namespace {
std::atomic<int> gResetRequests{0};
std::atomic<int> gSwitches{0};
long HostAsioMessage(long selector, long value, void*, double*) {
  if (selector == kAsioSelectorSupported) return value == kAsioResetRequest ? 1 : 0;
  if (selector == kAsioResetRequest) gResetRequests.fetch_add(1);
  return 0;
}
void HostBufferSwitch(long, ASIOBool) { gSwitches.fetch_add(1); }
bool WaitResets(int want) {
  for (int i = 0; i < 200 && gResetRequests.load() < want; ++i) Sleep(10);
  Sleep(50);  // and no extra ones
  return gResetRequests.load() == want;
}
}  // namespace

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // A saved Slot Table (Control Panel Save) as the Master's starting point: the default layout with
  // a renamed input and a chosen HW input device. WINHOOKAUDIO_SLOTS_JSON keeps the real
  // %ProgramData% file out of the test.
  char savedPath[MAX_PATH];
  GetTempPathA(MAX_PATH, savedPath);
  strcat_s(savedPath, "winhookaudio-host-sample-slots.json");
  const char* savedDevice = "{0.0.1.00000000}.{saved-capture-device}";
  {
    WHASlotTable saved{};
    saved.masterInCount = 2;
    saved.masterOutCount = 2;
    saved.masterIn[0].type = SLOT_HW;
    saved.masterIn[0].enabled = 1;
    SetSlotName(saved.masterIn[0], "Saved Mic");
    SetSlotName(saved.masterIn[1], "- empty -");
    saved.masterOut[0].type = SLOT_HW;
    saved.masterOut[0].enabled = 1;
    SetSlotName(saved.masterOut[0], "Main L");
    SetSlotName(saved.masterOut[1], "- empty -");
    TruncateCopy(saved.general.hwCaptureId, kEndpointIdLen, savedDevice);
    saved.version = 5;
    std::string error;
    check("saved table is valid", ValidateSlots(saved, &error));
    FILE* f = nullptr;
    const std::string json = SerializeSlots(saved);
    if (fopen_s(&f, savedPath, "wb") == 0 && f) {
      std::fwrite(json.data(), 1, json.size(), f);
      std::fclose(f);
    }
    SetEnvironmentVariableA("WINHOOKAUDIO_SLOTS_JSON", savedPath);
  }

  // Master: init -> getChannels -> getChannelInfo -> createBuffers -> start -> bufferSwitch
  {
    auto* master = new WinHookMasterASIO();
    check("master init returns ASIOTrue", master->init(nullptr) == ASIOTrue);
    {
      ASIOChannelInfo first{};
      first.channel = 0;
      first.isInput = ASIOTrue;
      HANDLE savedMap = OpenFileMappingA(FILE_MAP_READ, FALSE, shm::kSlotTableName + 7);
      auto* live = savedMap ? static_cast<const WHASlotTable*>(MapViewOfFile(savedMap, FILE_MAP_READ, 0, 0, shm::kSlotTableSize)) : nullptr;
      check("fresh Slot Table loaded from the saved file (names, HW device survive restart)",
            master->getChannelInfo(&first) == ASE_OK && std::strcmp(first.name, "Saved Mic") == 0 && live &&
                std::strcmp(live->general.hwCaptureId, savedDevice) == 0 && live->version == 5);
      if (live) UnmapViewOfFile(live);
      if (savedMap) CloseHandle(savedMap);
    }
    long in = 0, out = 0;
    ASIOError err = master->getChannels(&in, &out);
    check("master getChannels 1..512", err == ASE_OK && in >= 1 && in <= 512 && out >= 1 && out <= 512);
    bool allNames = true;
    for (long i = 0; i < in; ++i) {
      ASIOChannelInfo ci{}; ci.channel = i; ci.isInput = ASIOTrue;
      if (master->getChannelInfo(&ci) != ASE_OK || ci.name[0] == '\0' || ci.type != ASIOSTFloat32LSB) { allNames = false; break; }
    }
    check("master all input names, Float32LSB", allNames);
    double sr = 0;
    err = master->getSampleRate(&sr);
    check("master getSampleRate", err == ASE_OK && (sr == 44100 || sr == 48000 || sr == 96000));
    check("master canSampleRate: Master Clock only", master->canSampleRate(sr) == ASE_OK &&
                                                          master->canSampleRate(sr == 48000 ? 44100 : 48000) == ASE_NoClock);
    long minSz = 0, maxSz = 0, pref = 0, gran = -1;
    err = master->getBufferSize(&minSz, &maxSz, &pref, &gran);
    check("master getBufferSize = Master Clock buffer", err == ASE_OK && minSz == pref && maxSz == pref && gran == 0 &&
                                                            pref >= 64 && pref <= 1024);
    ASIOClockSource clocks[2]{};
    long nClocks = 2;
    check("master getClockSources: one current", master->getClockSources(clocks, &nClocks) == ASE_OK && nClocks == 1 &&
                                                     clocks[0].isCurrentSource == ASIOTrue);
    ASIOSamples pos{};
    ASIOTimeStamp ts{};
    check("master getSamplePosition before start", master->getSamplePosition(&pos, &ts) == ASE_SPNotAdvancing);
    check("master start before createBuffers rejected", master->start() == ASE_InvalidMode);

    ASIOCallbacks cbs{};
    cbs.bufferSwitch = HostBufferSwitch;
    cbs.asioMessage = HostAsioMessage;
    ASIOBufferInfo infos[2]{};
    infos[0].isInput = ASIOTrue; infos[0].channelNum = 0;
    infos[1].isInput = ASIOFalse; infos[1].channelNum = 0;
    check("master createBuffers wrong size rejected", master->createBuffers(infos, 2, pref * 2, &cbs) == ASE_InvalidMode);
    ASIOBufferInfo bad = infos[0];
    bad.channelNum = in;
    check("master createBuffers bad channel rejected", master->createBuffers(&bad, 1, pref, &cbs) == ASE_InvalidParameter);
    err = master->createBuffers(infos, 2, pref, &cbs);
    check("master createBuffers fills double buffers", err == ASE_OK && infos[0].buffers[0] && infos[0].buffers[1] &&
                                                           infos[1].buffers[0] && infos[1].buffers[1] &&
                                                           infos[0].buffers[0] != infos[0].buffers[1]);
    ASIOChannelInfo active{}; active.channel = 0; active.isInput = ASIOTrue;
    master->getChannelInfo(&active);
    check("master channel with buffers is active", active.isActive == ASIOTrue);
    err = master->start();
    check("master start", err == ASE_OK);
    Sleep(300);
    const int switches = gSwitches.load();
    std::printf("  bufferSwitch calls in 300 ms: %d (expected ~%.0f)\n", switches, 0.3 * sr / pref);
    check("master Master Clock drives bufferSwitch", switches > 0.3 * sr / pref * 0.7 && switches < 0.3 * sr / pref * 1.3);
    check("master getSamplePosition advances", master->getSamplePosition(&pos, &ts) == ASE_OK &&
                                                   FromAsio64(pos.hi, pos.lo) > 0 && FromAsio64(ts.hi, ts.lo) > 0);
    check("master outputReady not needed (directProcess)", master->outputReady() == ASE_NotPresent);
    check("master future: unknown selector", master->future(kAsioCanTimeInfo, nullptr) == ASE_InvalidParameter);

    // Another process (e.g. a Bridge popup) saves through the named Slot Table + TableChanged:
    // the Master must ask its own DAW to reset only when what that DAW sees changed.
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kSlotTableName + 7);
    HANDLE changed = OpenEventA(EVENT_MODIFY_STATE, FALSE, shm::kTableChangedName + 7);
    auto* other = map ? static_cast<WHASlotTable*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize)) : nullptr;
    check("other process opens Slot Table + TableChanged", other && changed);
    if (other && changed) {
      other->general.virtualBuffer = other->general.virtualBuffer == 256 ? 512 : 256;  // Per-Thing only
      SetEvent(changed);
      check("Per-Thing change from other process: no DAW reset", WaitResets(0));
      SetSlotName(other->masterIn[0], "Renamed by Bridge");
      SetEvent(changed);
      check("DAW-visible change from other process: one DAW reset", WaitResets(1));
      SetSlotName(other->masterIn[0], "Renamed again");
      SetEvent(changed);
      check("Second change before DAW re-query: no duplicate reset", WaitResets(1));
      long in2 = 0, out2 = 0;
      master->getChannels(&in2, &out2);  // DAW handles the reset and re-queries
      other->masterOutCount = other->masterOutCount > 1 ? other->masterOutCount - 1 : 2;
      SetEvent(changed);
      check("Change after DAW re-query: reset again", WaitResets(2));
      master->getChannels(&in2, &out2);
      // The HW device is opened at start and sets the latencies: choosing another one resets too.
      TruncateCopy(other->general.hwCaptureId, kEndpointIdLen, "{0.0.1.00000000}.{another-device}");
      SetEvent(changed);
      check("HW device change from other process: DAW reset", WaitResets(3));
    }
    if (other) UnmapViewOfFile(other);
    if (map) CloseHandle(map);
    if (changed) CloseHandle(changed);

    err = master->stop();
    const int afterStop = gSwitches.load();
    Sleep(50);
    check("master stop: no bufferSwitch after stop", err == ASE_OK && gSwitches.load() == afterStop);
    check("master disposeBuffers", master->disposeBuffers() == ASE_OK);
    master->Release();
  }

  // Bridge without a Master: refused, with the reason
  {
    auto* alone = new WinHookBridgeASIO(0);
    check("bridge init without a Master returns ASIOFalse", alone->init(nullptr) == ASIOFalse);
    char msg[124] = {};
    alone->getErrorMessage(msg);
    check("bridge without a Master says to open it first", std::strstr(msg, "Open WinHookAudio Master") != nullptr);
    long in = -1, out = -1;
    check("bridge without a Master has no channels", alone->getChannels(&in, &out) == ASE_NotPresent);
    alone->Release();
  }

  // Bridge: 4 clients, 5th rejected (a Master is open)
  {
    auto* master = new WinHookMasterASIO();
    check("master open for the Bridge clients", master->init(nullptr) == ASIOTrue);
    WinHookBridgeASIO* bridges[5] = {};
    bool bridgePass = true;
    for (int i = 0; i < 4; ++i) {
      bridges[i] = new WinHookBridgeASIO(0);
      if (bridges[i]->init(nullptr) != ASIOTrue) { bridgePass = false; std::printf("bridge %d init FAIL\n", i); }
    }
    check("bridge 4 clients init", bridgePass);
    bridges[4] = new WinHookBridgeASIO(0);
    check("bridge 5th client init returns ASIOFalse", bridges[4]->init(nullptr) == ASIOFalse);
    char msg[124] = {};
    bridges[4]->getErrorMessage(msg);
    check("bridge 5th client explains why", std::strstr(msg, "full") != nullptr);
    bridges[4]->Release();
    bridges[4] = nullptr;
    // An app closes the driver: its place is free for the next app (no "full" after restarts).
    bridges[1]->Release();
    bridges[1] = new WinHookBridgeASIO(0);
    check("bridge place given back on close, next app gets it", bridges[1]->init(nullptr) == ASIOTrue);
    // An app that exited without closing (crash): its place is taken over when the Bridge is full.
    HANDLE sharedMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kBridgeSharedNames[0] + 7);
    auto* shared = sharedMap ? static_cast<WHABridgeShared*>(MapViewOfFile(sharedMap, FILE_MAP_ALL_ACCESS, 0, 0, shm::kBridgeSharedSize)) : nullptr;
    check("bridge region open", shared && CountBridgeClients(*shared) == 4);
    if (shared) {
      bridges[0]->Release();  // place 0 free ...
      bridges[0] = nullptr;
      shared->owner[0] = 0x7FFFFFFC;  // ... then held by an app that is gone (no such process)
      bridges[4] = new WinHookBridgeASIO(0);
      check("bridge place of an exited app taken over", bridges[4]->init(nullptr) == ASIOTrue &&
                                                            shared->owner[0] == static_cast<int32_t>(GetCurrentProcessId()) &&
                                                            CountBridgeClients(*shared) == 4);
      UnmapViewOfFile(shared);
    }
    if (sharedMap) CloseHandle(sharedMap);
    // A Save that changes what the Slave DAW sees (a Bridge channel added in the Master panel): the
    // Bridge asks its DAW to reset once, while streaming. A Per-Thing-only Save: no reset.
    {
      HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kSlotTableName + 7);
      auto* t = map ? static_cast<WHASlotTable*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize)) : nullptr;
      WinHookBridgeASIO* slave = bridges[1];
      ASIOCallbacks cbs{};
      cbs.bufferSwitch = HostBufferSwitch;
      cbs.asioMessage = HostAsioMessage;
      ASIOBufferInfo bufs[1]{};
      bufs[0].isInput = ASIOFalse;
      long bin = 0, bout = 0;
      slave->getChannels(&bin, &bout);  // what the DAW sees
      const bool streaming = t && slave->createBuffers(bufs, 1, static_cast<long>(t->general.asioBuffer), &cbs) == ASE_OK &&
                             slave->start() == ASE_OK;
      check("bridge streams for the Save checks", streaming);
      if (streaming) {
        gResetRequests = 0;
        t->general.virtualBuffer = t->general.virtualBuffer == 256 ? 512 : 256;
        ++t->version;
        check("bridge: Save the DAW cannot see -> no reset", WaitResets(0));
        const uint32_t k = t->masterInCount;
        t->masterIn[k] = WHASlot{};
        t->masterIn[k].type = SLOT_BRIDGE1;
        t->masterIn[k].enabled = 1;
        t->masterIn[k].srcChannel = 3;  // Ch 4: the app's outputs grow to 4
        t->masterInCount = k + 1;
        ++t->version;
        check("bridge: Save adding Bridge1 Ch4 -> one DAW reset", WaitResets(1));
        long in4 = 0, out4 = 0;
        slave->getChannels(&in4, &out4);  // the DAW handles the reset and re-queries
        check("bridge: after the reset the DAW sees 4 outputs", out4 == 4);
        ++t->version;  // saved again, nothing changed for the DAW
        check("bridge: unchanged Save after re-query -> no reset", WaitResets(1));
        slave->stop();
        slave->disposeBuffers();
        t->masterInCount = k;
        ++t->version;
      }
      if (t) UnmapViewOfFile(t);
      if (map) CloseHandle(map);
    }
    long in = 0, out = 0;
    ASIOError err = bridges[1]->getChannels(&in, &out);
    check("bridge getChannels: at least a stereo pair", err == ASE_OK && in >= 2 && out >= 2);
    ASIOChannelInfo ci{}; ci.channel = 0; ci.isInput = ASIOTrue;
    check("bridge getChannelInfo", bridges[1]->getChannelInfo(&ci) == ASE_OK);
    check("bridge outputReady not needed", bridges[1]->outputReady() == ASE_NotPresent);
    for (int i = 0; i < 5; ++i) if (bridges[i]) bridges[i]->Release();
    master->Release();
  }

  std::printf("{\"schema_version\":1,\"operation\":\"host_sample\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
