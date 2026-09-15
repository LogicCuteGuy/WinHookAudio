#include <cstdio>
#include <cstring>
#include "WinHookMasterASIO.h"
#include "WinHookBridgeASIO.h"

using namespace wha;

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
    err = master->createBuffers(nullptr, 0, pref, &cbs);
    check("master createBuffers", err == ASE_OK);
    err = master->start();
    check("master start", err == ASE_OK);
    err = master->outputReady();
    check("master outputReady (bufferSwitch memcpy + SetEvent)", err == ASE_OK);
    err = master->controlPanel();
    check("master controlPanel stub", err == ASE_OK);
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
