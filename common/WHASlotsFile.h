#pragma once

// WHASlotsFile — where the Control Panel saves the Slot Table, and loading it back.
// Vocabulary: Slot Table, Control Panel.
// The Master creates the Slot Table from this file when no process holds it yet, so routing, names
// and HW devices survive a DAW restart. WINHOOKAUDIO_SLOTS_JSON overrides the path (tests).

#include <windows.h>

#include <cstdio>
#include <string>

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
  t.version = 1;
}

inline std::string SlotsJsonPath() {
  char buf[MAX_PATH] = {};
  if (GetEnvironmentVariableA("WINHOOKAUDIO_SLOTS_JSON", buf, sizeof(buf)) > 0 && buf[0]) return buf;
  const DWORD n = ExpandEnvironmentStringsA(shm::kSlotsJsonPath, buf, sizeof(buf));
  return n > 0 && n <= sizeof(buf) ? std::string(buf) : std::string();
}

// A valid saved table, or false (absent, unreadable, or invalid: the caller keeps its defaults).
inline bool LoadSlotsFile(WHASlotTable& table, std::string* error) {
  const std::string path = SlotsJsonPath();
  FILE* f = nullptr;
  if (path.empty() || fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
    if (error) *error = "no " + path;
    return false;
  }
  std::string json;
  char chunk[4096];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) json.append(chunk, n);
  std::fclose(f);
  WHASlotTable loaded{};
  if (!DeserializeSlots(json, loaded, error)) return false;
  table = loaded;
  if (table.version == 0) table.version = 1;
  return true;
}

}  // namespace wha
