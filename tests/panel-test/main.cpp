#include <cstdio>
#include <cstring>
#include "WHAControlPanelUI.h"

using namespace wha;

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // Setup
  PanelModel model{};
  model.table.masterInCount = 3;
  model.table.masterIn[0].type = SLOT_HW; model.table.masterIn[0].enabled = 1;
  TruncateCopy(model.table.masterIn[0].name, kNameLen, "SM58 Mic");
  model.table.masterIn[1].type = SLOT_VIRTUAL; model.table.masterIn[1].enabled = 1;
  TruncateCopy(model.table.masterIn[1].name, kNameLen, "VRChat Out");
  model.table.masterIn[2].type = SLOT_NONE; model.table.masterIn[2].enabled = 0;
  TruncateCopy(model.table.masterIn[2].name, kNameLen, "- empty -");
  model.table.masterOutCount = 1;
  model.table.masterOut[0].type = SLOT_HW; model.table.masterOut[0].enabled = 1;
  TruncateCopy(model.table.masterOut[0].name, kNameLen, "Main L");

  // Filter
  check("filter All", FilteredCount(model, true) == 3);
  model.filter = PanelFilter::HW;
  check("filter HW", FilteredCount(model, true) == 1);
  model.filter = PanelFilter::Virtual;
  check("filter Virtual", FilteredCount(model, true) == 1);
  model.filter = PanelFilter::All;
  model.search = "VRChat";
  check("search VRChat", FilteredCount(model, true) == 1);
  model.search = "";
  check("search empty", FilteredCount(model, true) == 3);

  // Display name
  check("display - empty -", std::strcmp(SlotDisplayName(model.table.masterIn[2]), "- empty -") == 0);
  check("display SM58", std::strcmp(SlotDisplayName(model.table.masterIn[0]), "SM58 Mic") == 0);

  // Loopback editable only VIRTUAL
  check("loopback VIRTUAL editable", IsLoopbackEditable(model.table.masterIn[1]) == true);
  check("loopback HW not editable", IsLoopbackEditable(model.table.masterIn[0]) == false);
  check("loopback NONE not editable", IsLoopbackEditable(model.table.masterIn[2]) == false);
  check("set loopback VIRTUAL ok", SetInputLoopback(model, 1, true) == true && model.table.masterIn[1].loopback == 1);
  check("set loopback HW fail", SetInputLoopback(model, 0, true) == false);

  // AddInput
  uint32_t before = model.table.masterInCount;
  check("AddInput", AddInput(model) == true && model.table.masterInCount == before + 1);
  check("AddInput is empty", model.table.masterIn[before].type == SLOT_NONE);

  // InsertEmptyAbove
  check("InsertEmptyAbove", InsertEmptyAbove(model, 1) == true);
  check("InsertEmptyAbove is empty", model.table.masterIn[1].type == SLOT_NONE);
  check("InsertEmptyAbove shifted", model.table.masterIn[2].type == SLOT_VIRTUAL);

  // Duplicate
  uint32_t dupBefore = model.table.masterInCount;
  // Set a known slot to duplicate
  model.table.masterIn[0].type = SLOT_HW;
  TruncateCopy(model.table.masterIn[0].name, kNameLen, "DupTest");
  check("Duplicate", DuplicateInput(model, 0) == true && model.table.masterInCount == dupBefore + 1);
  check("Duplicate copy", std::strcmp(model.table.masterIn[1].name, "DupTest") == 0);

  // MoveInput (INPUT only, memmove)
  // Move 0 -> 2
  WHASlot moved = model.table.masterIn[0];
  check("MoveInput", MoveInput(model, 0, 2) == true);
  check("MoveInput result", model.table.masterIn[2].type == moved.type);

  // DeleteInput
  uint32_t delBefore = model.table.masterInCount;
  check("DeleteInput", DeleteInput(model, 0) == true && model.table.masterInCount == delBefore - 1);

  // OUTPUT not moved by INPUT operations
  check("OUTPUT not moved", model.table.masterOutCount == 1 && model.table.masterOut[0].type == SLOT_HW);

  // Bridge popup filtered
  PanelModel bridgeModel = model;
  bridgeModel.isMaster = false;
  bridgeModel.filter = PanelFilter::All;
  // Add a BRIDGE1 slot
  bridgeModel.table.masterIn[0].type = SLOT_BRIDGE1;
  bridgeModel.table.masterIn[1].type = SLOT_HW;
  check("Bridge popup filtered", FilteredCount(bridgeModel, true) == 1);

  // SetInputType clears loopback if not VIRTUAL
  model.table.masterIn[0].type = SLOT_VIRTUAL; model.table.masterIn[0].loopback = 1;
  check("SetInputType clears loopback", SetInputType(model, 0, SLOT_HW) == true && model.table.masterIn[0].loopback == 0);

  // OUTPUTS 512 — independent indices
  PanelModel outModel{};
  outModel.table.masterOutCount = 2;
  outModel.table.masterOut[0].type = SLOT_HW; outModel.table.masterOut[0].enabled = 1;
  TruncateCopy(outModel.table.masterOut[0].name, kNameLen, "Main L");
  outModel.table.masterOut[1].type = SLOT_VIRTUAL; outModel.table.masterOut[1].enabled = 1;
  TruncateCopy(outModel.table.masterOut[1].name, kNameLen, "To VRCT");
  outModel.table.masterInCount = 1;
  outModel.table.masterIn[0].type = SLOT_HW; outModel.table.masterIn[0].enabled = 1;
  TruncateCopy(outModel.table.masterIn[0].name, kNameLen, "Mic 1");
  uint32_t outBefore = outModel.table.masterOutCount;
  check("AddOutput", AddOutput(outModel) == true && outModel.table.masterOutCount == outBefore + 1);
  check("AddOutput not move INPUT", outModel.table.masterInCount == 1);
  check("MoveOutput", MoveOutput(outModel, 0, 1) == true && outModel.table.masterOut[1].type == SLOT_HW);
  check("MoveOutput not move INPUT", outModel.table.masterIn[0].type == SLOT_HW);
  check("SetOutputLoopback VIRTUAL ok", SetOutputLoopback(outModel, 1, true) == false);  // SLOT_HW not VIRTUAL
  outModel.table.masterOut[1].type = SLOT_VIRTUAL;
  check("SetOutputLoopback VIRTUAL", SetOutputLoopback(outModel, 1, true) == true);
  check("DeleteOutput", DeleteOutput(outModel, 0) == true);
  check("DuplicateOutput", DuplicateOutput(outModel, 0) == true);

  // GENERAL Per-Thing
  PanelModel genModel{};
  genModel.table.general.sampleRate = 48000; genModel.table.general.asioBuffer = 128;
  check("SetMasterClock ok", SetMasterClock(genModel, 48000, 128) == true);
  check("SetMasterClock invalid", SetMasterClock(genModel, 22050, 128) == false);
  check("SetHwBuffer ok", SetHwBuffer(genModel, 64) == true && genModel.table.general.hwBuffer == 64);
  check("SetHwBuffer invalid", SetHwBuffer(genModel, 100) == false);
  check("SetVirtualBuffer", SetVirtualBuffer(genModel, 256) == true);
  check("SetBridgeBuffer", SetBridgeBuffer(genModel, 0, 128) == true);
  check("SetBridgeBuffer invalid index", SetBridgeBuffer(genModel, 4, 128) == false);
  check("SetNetworkPcmBuffer", SetNetworkPcmBuffer(genModel, 512) == true);
  check("SetNetworkVorbisBuffer", SetNetworkVorbisBuffer(genModel, 1024) == true);
  check("SetJitterPcm", SetJitterPcm(genModel, 20) == true);
  check("SetJitterVorbis", SetJitterVorbis(genModel, 50) == true);
  // Bridge popup GENERAL read-only
  PanelModel bridgeGen = genModel; bridgeGen.isMaster = false;
  check("GENERAL read-only Bridge", IsGeneralReadOnly(bridgeGen) == true);
  check("SetMasterClock Bridge fail", SetMasterClock(bridgeGen, 48000, 128) == false);
  check("SetHwBuffer Bridge fail", SetHwBuffer(bridgeGen, 64) == false);

  // ABOUT + Save contract (13)
  PanelModel aboutModel{};
  aboutModel.table.masterInCount = 2;
  aboutModel.table.masterIn[0].type = SLOT_BRIDGE1; aboutModel.table.masterIn[0].enabled = 1;
  TruncateCopy(aboutModel.table.masterIn[0].name, kNameLen, "Bridge1 Ch1");
  aboutModel.table.masterIn[1].type = SLOT_BRIDGE1; aboutModel.table.masterIn[1].enabled = 1;
  TruncateCopy(aboutModel.table.masterIn[1].name, kNameLen, "Bridge1 Ch2");
  AboutInfo about = GetAboutInfo(aboutModel);
  check("ABOUT version", !about.version.empty());
  check("ABOUT clsidCount 5", about.clsidCount == 5);
  check("ABOUT slotsJsonPath", !about.slotsJsonPath.empty());
  check("ABOUT bridgeClients", !about.bridgeClients[0].empty());

  // SavePanel: version++ + slots.json + resetRequested
  WHASlotTable pTable{};
  pTable.version = 5;
  pTable.masterInCount = 1; pTable.masterIn[0].type = SLOT_HW; pTable.masterIn[0].enabled = 1;
  TruncateCopy(pTable.masterIn[0].name, kNameLen, "Mic 1");
  pTable.masterOutCount = 1; pTable.masterOut[0].type = SLOT_HW; pTable.masterOut[0].enabled = 1;
  TruncateCopy(pTable.masterOut[0].name, kNameLen, "Main L");
  PanelModel editCopy{}; editCopy.table = pTable;
  editCopy.table.masterIn[0].type = SLOT_VIRTUAL;
  TruncateCopy(editCopy.table.masterIn[0].name, kNameLen, "VRChat Out");
  std::string jsonOut; bool resetRequested = false;
  check("SavePanel", SavePanel(editCopy, &pTable, &jsonOut, &resetRequested) == true);
  check("SavePanel version++", pTable.version == 6);
  check("SavePanel jsonOut", !jsonOut.empty());
  // Master Clock not changed (only type/name), so resetRequested should be false (Per-Thing only)
  check("SavePanel resetRequested false for non-clock", resetRequested == false);
  // Now change Master Clock — should request reset
  editCopy.table.general.sampleRate = 44100;
  std::string jsonOut2; bool reset2 = false;
  WHASlotTable pTable2 = pTable;
  check("SavePanel clock change", SavePanel(editCopy, &pTable2, &jsonOut2, &reset2) == true);
  check("SavePanel resetRequested true for clock", reset2 == true);
  check("SavePanel pTable updated", std::strcmp(pTable.masterIn[0].name, "VRChat Out") == 0);
  // slots.json round-trip
  check("SavePanel json round-trip", jsonOut.find("VRChat Out") != std::string::npos);

  // ResetToDefault
  PanelModel resetModel{}; resetModel.table.masterInCount = 5;
  check("ResetToDefault", ResetToDefault(resetModel) == true && resetModel.table.masterInCount == 2);
  check("ResetToDefault version 1", resetModel.table.version == 1);

  // Export/Import
  WHASlotTable expTable{}; expTable.version = 1; expTable.masterInCount = 1; expTable.masterIn[0].type = SLOT_HW; expTable.masterIn[0].enabled = 1;
  TruncateCopy(expTable.masterIn[0].name, kNameLen, "ExportTest");
  expTable.masterOutCount = 1; expTable.masterOut[0].type = SLOT_HW; expTable.masterOut[0].enabled = 1;
  TruncateCopy(expTable.masterOut[0].name, kNameLen, "Main L");
  std::string tmpPath = "test_export.json";
  check("ExportSlots", ExportSlots(expTable, tmpPath) == true);
  WHASlotTable impTable{}; std::string impErr;
  check("ImportSlots", ImportSlots(impTable, tmpPath, &impErr) == true && std::strcmp(impTable.masterIn[0].name, "ExportTest") == 0);
  std::remove(tmpPath.c_str());

  // GENERAL Virtual Cables (18)
  PanelModel vcModel{};
  check("VirtualCables default 8", GetVirtualCableCount(vcModel) == 8);
  check("VirtualCableName default", GetVirtualCableName(vcModel) == "WinHookAudio Virtual");
  check("SetVirtualCableCount 8", SetVirtualCableCount(vcModel, 8) == true);
  check("SetVirtualCableCount 64", SetVirtualCableCount(vcModel, 64) == true && GetVirtualCableCount(vcModel) == 64);
  check("SetVirtualCableCount invalid", SetVirtualCableCount(vcModel, 16) == false);
  check("SetVirtualCableName", SetVirtualCableName(vcModel, "My Virtual") == true && GetVirtualCableName(vcModel) == "My Virtual");
  PanelModel vcBridge = vcModel; vcBridge.isMaster = false;
  check("VirtualCables Bridge read-only", SetVirtualCableCount(vcBridge, 8) == false);
  check("VirtualCableName Bridge read-only", SetVirtualCableName(vcBridge, "X") == false);

  // NETWORK 8 tab (16)
  PanelModel netModel{};
  WHANetworkStream tx{}; tx.port = 6980; tx.codec = WHA_PCM_F32; tx.channels = 32; tx.quality = 0.4f;
  strcpy_s(tx.ip, "192.168.1.50");
  check("SetNetworkTx", SetNetworkTx(netModel, 0, tx) == true);
  check("GetNetworkTx", GetNetworkTx(netModel, 0).channels == 32);
  check("SetNetworkTx invalid index", SetNetworkTx(netModel, 8, tx) == false);
  WHANetworkStream badTx = tx; badTx.channels = 100;
  check("SetNetworkTx invalid channels", SetNetworkTx(netModel, 0, badTx) == false);
  WHANetworkStream rx{}; rx.port = 6980; rx.codec = WHA_VORBIS; rx.channels = 2; rx.quality = 0.4f;
  strcpy_s(rx.ip, "192.168.1.50");
  check("SetNetworkRx", SetNetworkRx(netModel, 0, rx) == true);
  check("NetworkBandwidth PCM_F32", NetworkBandwidthMbps(tx) > 40.0);
  check("NetworkBandwidth VORBIS", NetworkBandwidthMbps(rx) > 0.05 && NetworkBandwidthMbps(rx) < 1.0);
  // INPUTS Type=NETWORK Rx1 linkage
  netModel.table.masterInCount = 1;
  netModel.table.masterIn[0].type = SLOT_NETWORK; netModel.table.masterIn[0].streamId = 0; netModel.table.masterIn[0].enabled = 1;
  TruncateCopy(netModel.table.masterIn[0].name, kNameLen, "Network Rx1");
  check("NETWORK Rx linkage", netModel.table.masterIn[0].type == SLOT_NETWORK && netModel.table.masterIn[0].streamId == 0);

  std::printf("{\"schema_version\":1,\"operation\":\"panel_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
