#pragma once

// WHASlotsFile — where the Control Panel saves the Slot Table, and loading it back.
// Vocabulary: Slot Table, Control Panel.
// The Master creates the Slot Table from routes.yml + settings.yml (WHAConfigYaml.h) when no process
// holds it yet, so routing, names and HW devices survive a DAW restart. WINHOOKAUDIO_CONFIG_DIR
// overrides their folder and WINHOOKAUDIO_SLOTS_JSON the old slots.json path (tests).

#include <windows.h>

#include <cstdio>
#include <string>

#include "WHAConfigYaml.h"
#include "WHASharedMemory.h"
#include "WHASlotsJson.h"

namespace wha {

// The Master's first-run Slot Table (no saved file): HW Mic 1 / Main L, one empty slot each way,
// every GENERAL default. `t` may be a freshly mapped (zeroed) table, or one a creator pre-filled:
// its HW fields are kept.
inline void FillDefaultSlotTable(WHASlotTable& t) {
  t.masterInCount = 2;
  t.masterOutCount = 2;
  t.masterIn[0].type = SLOT_HW;
  t.masterIn[0].enabled = 1;
  TruncateCopy(t.masterIn[0].name, kNameLen, "Mic 1");
  t.masterIn[1].type = SLOT_NONE;
  t.masterIn[1].enabled = 0;
  TruncateCopy(t.masterIn[1].name, kNameLen, "- empty -");
  t.masterOut[0].type = SLOT_HW;
  t.masterOut[0].enabled = 1;
  TruncateCopy(t.masterOut[0].name, kNameLen, "Main L");
  t.masterOut[1].type = SLOT_NONE;
  t.masterOut[1].enabled = 0;
  TruncateCopy(t.masterOut[1].name, kNameLen, "- empty -");
  // All GENERAL defaults (a zeroed table fails validation: bitDepth 0, virtualCables 0, bridge
  // buffers 0, so the Control Panel could never Save). HW fields a creator already wrote are kept
  // (a zero hwBuffer in a fresh mapping is "untouched": the default; "" device = Windows default).
  WHAGeneral general{};
  if (t.general.hwBuffer) general.hwBuffer = t.general.hwBuffer;
  TruncateCopy(general.hwRenderId, kEndpointIdLen, t.general.hwRenderId);
  TruncateCopy(general.hwCaptureId, kEndpointIdLen, t.general.hwCaptureId);
  t.general = general;
  t.general.sampleRate = kMasterClockRateDefault;
  t.general.asioBuffer = kMasterClockBufferDefault;
  for (uint32_t i = 0; i < kNetStreams; ++i) t.netTx[i] = t.netRx[i] = WHANetworkStream{};  // zeroed: invalid
  for (int c = 0; c < kVirtualSlotCables; ++c) CableSetting(t, c) = WHACableSetting{};  // zeroed: 0 channels, invalid
  t.version = 1;
}

// The old one-file format (read only when there is no routes.yml / settings.yml).
inline std::string SlotsJsonPath() {
  char buf[MAX_PATH] = {};
  if (GetEnvironmentVariableA("WINHOOKAUDIO_SLOTS_JSON", buf, sizeof(buf)) > 0 && buf[0]) return buf;
  const DWORD n = ExpandEnvironmentStringsA(shm::kSlotsJsonPath, buf, sizeof(buf));
  return n > 0 && n <= sizeof(buf) ? std::string(buf) : std::string();
}

// The folder of routes.yml and settings.yml; WINHOOKAUDIO_CONFIG_DIR overrides it (tests). Accepts
// %VARIABLES%. Empty when it cannot be expanded.
inline std::string ConfigDir(const std::string& override = std::string()) {
  char buf[MAX_PATH] = {};
  std::string dir = override;
  if (dir.empty() && GetEnvironmentVariableA("WINHOOKAUDIO_CONFIG_DIR", buf, sizeof(buf)) > 0 && buf[0]) dir = buf;
  if (dir.empty()) dir = shm::kConfigDir;
  const DWORD n = ExpandEnvironmentStringsA(dir.c_str(), buf, sizeof(buf));
  if (n == 0 || n > sizeof(buf)) return std::string();
  dir = buf;
  while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
  return dir;
}
inline std::string RoutesPath(const std::string& dir) { return dir + "\\" + shm::kRoutesFile; }
inline std::string SettingsPath(const std::string& dir) { return dir + "\\" + shm::kSettingsFile; }

inline bool ReadWholeFile(const std::string& path, std::string& out) {
  FILE* f = nullptr;
  if (path.empty() || fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
  out.clear();
  char chunk[4096];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) out.append(chunk, n);
  std::fclose(f);
  return true;
}

// The saved Slot Table: routes.yml + settings.yml (a missing one of the two = its defaults), else an
// old slots.json. False when nothing is saved or a file is invalid: the caller keeps its defaults.
inline bool LoadConfigFiles(WHASlotTable& table, std::string* error) {
  const std::string dir = ConfigDir();
  std::string routes, settings;
  const bool haveRoutes = !dir.empty() && ReadWholeFile(RoutesPath(dir), routes);
  const bool haveSettings = !dir.empty() && ReadWholeFile(SettingsPath(dir), settings);
  WHASlotTable loaded{};
  if (haveRoutes || haveSettings) {
    FillDefaultSlotTable(loaded);
    std::string err;
    if (haveRoutes && !DeserializeRoutes(routes, loaded, &err)) {
      if (error) *error = std::string(shm::kRoutesFile) + " " + err;
      return false;
    }
    if (haveSettings && !DeserializeSettings(settings, loaded, &err)) {
      if (error) *error = std::string(shm::kSettingsFile) + " " + err;
      return false;
    }
    if (!ValidateSlots(loaded, error)) return false;
  } else {
    std::string json;
    const std::string path = SlotsJsonPath();
    if (!ReadWholeFile(path, json)) {
      if (error) *error = "no " + RoutesPath(dir) + ", " + SettingsPath(dir) + " or " + path;
      return false;
    }
    if (!DeserializeSlots(json, loaded, error)) return false;
  }
  table = loaded;
  if (table.version == 0) table.version = 1;
  return true;
}

}  // namespace wha
