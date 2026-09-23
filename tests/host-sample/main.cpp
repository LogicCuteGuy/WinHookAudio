#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include "WinHookMasterASIO.h"
#include "WinHookBridgeASIO.h"

using namespace wha;

namespace {
std::atomic<int> gResetRequests{0};
int32_t HostAsioMessage(int32_t selector, int32_t, void*, double*) {
  if (selector == kAsioResetRequest) gResetRequests.fetch_add(1);
  return 1;
}
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

  // Master: CoCreateInstance -> init -> getChannels -> getChannelInfo -> createBuffers -> start -> bufferSwitch
  {
    auto* master = new WinHookMasterASIO();
    ASIOError err = master->init(nullptr);
    check("master init", err == ASE_OK);
    int32_t in = 0, out = 0;
    err = master->getChannels(&in, &out);
    check("master getChannels 1..512", err == ASE_OK && in >= 1 && in <= 512 && out >= 1 && out <= 512);
    ASIOChannelInfo info{};
    info.channel = 0; info.isInput = 1;
    err = master->getChannelInfo(&info);
    check("master getChannelInfo ch0", err == ASE_OK && (std::strcmp(info.name, "- empty -") == 0 || info.name[0] != '\0'));
    // Check all channels have valid names
    bool allNames = true;
    for (int32_t i = 0; i < in; ++i) {
      ASIOChannelInfo ci{}; ci.channel = i; ci.isInput = 1;
      if (master->getChannelInfo(&ci) != ASE_OK) { allNames = false; break; }
      if (ci.name[0] == '\0') { allNames = false; break; }
    }
    check("master all input names", allNames);
    double sr = 0;
    err = master->getSampleRate(&sr);
    check("master getSampleRate", err == ASE_OK && (sr == 44100 || sr == 48000 || sr == 96000));
    int32_t minSz, maxSz, pref, gran;
    err = master->getBufferSize(&minSz, &maxSz, &pref, &gran);
    check("master getBufferSize", err == ASE_OK && pref >= 64 && pref <= 1024);
    ASIOCallbacks cbs{};
    cbs.asioMessage = HostAsioMessage;
    err = master->createBuffers(nullptr, 0, pref, &cbs);
    check("master createBuffers", err == ASE_OK);
    err = master->start();
    check("master start", err == ASE_OK);
    err = master->outputReady();
    check("master outputReady (bufferSwitch memcpy + SetEvent)", err == ASE_OK);
    err = master->controlPanel();
    check("master controlPanel stub", err == ASE_OK);

    // Another process (e.g. a Bridge popup) saves through the named Slot Table + TableChanged:
    // the Master must ask its own DAW to reset only when what that DAW sees changed.
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm::kSlotTableName + 7);
    HANDLE changed = OpenEventA(EVENT_MODIFY_STATE, FALSE, shm::kTableChangedName + 7);
    auto* other = map ? static_cast<WHASlotTable*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, shm::kSlotTableSize)) : nullptr;
    check("other process opens Slot Table + TableChanged", other && changed);
    if (other && changed) {
      other->general.hwBuffer = other->general.hwBuffer == 64 ? 128 : 64;  // Per-Thing only
      SetEvent(changed);
      check("Per-Thing change from other process: no DAW reset", WaitResets(0));
      SetSlotName(other->masterIn[0], "Renamed by Bridge");
      SetEvent(changed);
      check("DAW-visible change from other process: one DAW reset", WaitResets(1));
      SetSlotName(other->masterIn[0], "Renamed again");
      SetEvent(changed);
      check("Second change before DAW re-query: no duplicate reset", WaitResets(1));
      int32_t in2 = 0, out2 = 0;
      master->getChannels(&in2, &out2);  // DAW handles the reset and re-queries
      other->masterOutCount = other->masterOutCount > 1 ? other->masterOutCount - 1 : 2;
      SetEvent(changed);
      check("Change after DAW re-query: reset again", WaitResets(2));
    }
    if (other) UnmapViewOfFile(other);
    if (map) CloseHandle(map);
    if (changed) CloseHandle(changed);

    err = master->stop();
    check("master stop", err == ASE_OK);
    master->Release();
  }

  // Bridge: 4 clients, 5th rejected
  {
    WinHookBridgeASIO* bridges[5] = {};
    bool bridgePass = true;
    for (int i = 0; i < 4; ++i) {
      bridges[i] = new WinHookBridgeASIO(0);
      ASIOError err = bridges[i]->init(nullptr);
      if (err != ASE_OK) { bridgePass = false; std::printf("bridge %d init FAIL %d\n", i, err); }
    }
    check("bridge 4 clients init", bridgePass);
    bridges[4] = new WinHookBridgeASIO(0);
    ASIOError err = bridges[4]->init(nullptr);
    check("bridge 5th client ASE_NotPresent", err == ASE_NotPresent);
    // getChannels counts BRIDGE1
    int32_t in = 0, out = 0;
    if (bridges[0]) {
      err = bridges[0]->getChannels(&in, &out);
      check("bridge getChannels", err == ASE_OK && in >= 0 && out >= 0);
      ASIOChannelInfo ci{}; ci.channel = 0; ci.isInput = 1;
      if (in > 0) {
        err = bridges[0]->getChannelInfo(&ci);
        check("bridge getChannelInfo", err == ASE_OK);
      }
      err = bridges[0]->outputReady();
      check("bridge outputReady", err == ASE_OK);
    }
    for (int i = 0; i < 5; ++i) if (bridges[i]) bridges[i]->Release();
  }

  std::printf("{\"schema_version\":1,\"operation\":\"host_sample\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
