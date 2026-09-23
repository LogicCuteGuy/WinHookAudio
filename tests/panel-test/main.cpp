#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "WHAControlPanelUI.h"
#include "WHASlotsJson.h"
#include "WHASlotsFile.h"
#include <memory>

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
  check("SetHwBuffer Auto (0 = device minimum)", SetHwBuffer(genModel, 0) == true && genModel.table.general.hwBuffer == 0);
  SetHwBuffer(genModel, 64);
  {  // The Master's first-run table from a freshly mapped (zeroed) Slot Table must be Saveable.
    auto fresh = std::make_unique<WHASlotTable>();
    std::memset(fresh.get(), 0, sizeof(WHASlotTable));
    FillDefaultSlotTable(*fresh);
    std::string err;
    const bool valid = ValidateSlots(*fresh, &err);
    if (!valid) std::printf("  default table invalid: %s\n", err.c_str());
    check("first-run default table validates (panel can Save)", valid && fresh->general.hwBuffer == 64 && fresh->general.bitDepth == 32);
    std::memset(fresh.get(), 0, sizeof(WHASlotTable));
    fresh->general.hwBuffer = 512;
    TruncateCopy(fresh->general.hwRenderId, kEndpointIdLen, "{pre-filled}");
    FillDefaultSlotTable(*fresh);
    check("first-run defaults keep pre-filled HW fields", fresh->general.hwBuffer == 512 &&
                                                            std::strcmp(fresh->general.hwRenderId, "{pre-filled}") == 0);
  }
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

  // GENERAL HW devices: IDs stored, empty = Windows default, too long rejected, Bridge read-only,
  // and a change asks the DAW to reset (devices are opened at start, latencies depend on them).
  {
    PanelModel dev = genModel;
    const WHASlotTable original = dev.table;
    const char* cable = "{0.0.0.00000000}.{7ecc6d7b-2fe2-4c5e-8d04-95893ed9ae03}";
    check("SetHwRenderDevice", SetHwRenderDevice(dev, cable) && std::strcmp(dev.table.general.hwRenderId, cable) == 0);
    check("HW device change is DAW-visible (reset)", DawVisibleChanged(original, dev.table));
    check("SetHwRenderDevice default (empty)", SetHwRenderDevice(dev, "") && dev.table.general.hwRenderId[0] == 0);
    check("back to default: no reset", !DawVisibleChanged(original, dev.table));
    const std::string tooLong(kEndpointIdLen, 'x');
    check("SetHwCaptureDevice too long rejected", !SetHwCaptureDevice(dev, tooLong.c_str()) && dev.table.general.hwCaptureId[0] == 0);
    check("SetHwCaptureDevice Bridge fail", !SetHwCaptureDevice(bridgeGen, cable));
    PanelModel hb = genModel;
    SetHwBuffer(hb, 512);
    check("HW buffer change is DAW-visible (reset)", DawVisibleChanged(genModel.table, hb.table) == (genModel.table.general.hwBuffer != 512));
    const std::vector<PanelEndpoint> list{{cable, "CABLE Input"}};
    check("EndpointName found", EndpointName(list, cable) && std::strcmp(EndpointName(list, cable), "CABLE Input") == 0);
    check("EndpointName missing -> nullptr", EndpointName(list, "{nope}") == nullptr);
    // JSON round trip keeps the IDs.
    SetHwRenderDevice(dev, cable);
    WHASlotTable back{};
    std::string err;
    dev.table.masterInCount = dev.table.masterOutCount = 1;  // a valid table has >= 1 slot each way
    const bool parsed = DeserializeSlots(SerializeSlots(dev.table), back, &err);
    if (!parsed) std::printf("  JSON error: %s\n", err.c_str());
    check("JSON round trip keeps HW device IDs", parsed &&
                                                     std::strcmp(back.general.hwRenderId, cable) == 0 && back.general.hwCaptureId[0] == 0);
  }

  // GENERAL HW status: requested versus actual, from the streaming Master's WHAMasterStats.
  {
    auto has = [](const std::vector<HwStatusLine>& lines, HwStatusLevel level, const char* text) {
      for (const HwStatusLine& l : lines)
        if (l.level == level && l.text.find(text) != std::string::npos) return true;
      return false;
    };
    auto dump = [](const std::vector<HwStatusLine>& lines) {
      for (const HwStatusLine& l : lines) std::printf("  [%d] %s\n", static_cast<int>(l.level), l.text.c_str());
    };
    const char* spk = "{0.0.0.00000000}.{aaaaaaaa-0000-0000-0000-000000000001}";
    const char* mic = "{0.0.1.00000000}.{aaaaaaaa-0000-0000-0000-000000000002}";
    PanelDevices devs;
    devs.render.push_back({spk, "Speakers"});
    devs.capture.push_back({mic, "Microphone"});
    WHAGeneral saved{};
    saved.hwBuffer = 64;

    std::vector<HwStatusLine> lines = HwStatusLines(nullptr, saved, &devs);
    check("HW status: not streaming", lines.size() == 1 && has(lines, HwStatusLevel::Info, "Not streaming"));

    WHAMasterStats st{};
    st.sampleRate = 48000;
    st.asioBuffer = 128;
    st.clockSource = CLOCK_HARDWARE;
    st.hwRequestValid = 1;
    st.hwRequestedPeriod = 64;
    st.hwOpen = 1;
    st.hwPeriod = 128;  // device minimum 2.67 ms
    st.hwFormat = HW_FORMAT_PCM24IN32;
    st.hwLatency = 480;
    TruncateCopy(st.hwRenderId, kStatsEndpointIdLen, spk);  // opened the Windows default: Speakers
    st.hwInOpen = 1;
    st.hwInPeriod = 128;
    st.hwInFormat = HW_FORMAT_PCM16;
    st.hwInLatency = 777;
    st.hwInDriftPpmMilli = -12500;
    st.hwInDriftEngaged = 1;
    TruncateCopy(st.hwCaptureId, kStatsEndpointIdLen, mic);
    lines = HwStatusLines(&st, saved, &devs);
    dump(lines);
    check("HW status: output device name, actual period, requested", has(lines, HwStatusLevel::Ok, "Output: Speakers") &&
                                                                         has(lines, HwStatusLevel::Ok, "period 128 frames (2.67 ms") &&
                                                                         has(lines, HwStatusLevel::Ok, "requested 64: device minimum"));
    check("HW status: output format + latency", has(lines, HwStatusLevel::Ok, "24-bit") && has(lines, HwStatusLevel::Ok, "latency 10.0 ms"));
    check("HW status: input name, format, drift", has(lines, HwStatusLevel::Ok, "Input: Microphone") &&
                                                      has(lines, HwStatusLevel::Ok, "16-bit") && has(lines, HwStatusLevel::Ok, "-12.5 ppm"));
    check("HW status: HW Master Clock", has(lines, HwStatusLevel::Info, "Master Clock: HW output"));
    check("HW status: nothing pending", !has(lines, HwStatusLevel::Warning, "DAW resets"));

    WHAMasterStats autoSt = st;
    autoSt.hwRequestedPeriod = 0;
    WHAGeneral savedAuto = saved;
    savedAuto.hwBuffer = 0;
    check("HW status: Auto period", has(HwStatusLines(&autoSt, savedAuto, &devs), HwStatusLevel::Ok, "Auto"));

    WHAGeneral changed = saved;
    changed.hwBuffer = 256;
    check("HW status: saved period not applied yet", has(HwStatusLines(&st, changed, &devs), HwStatusLevel::Warning, "DAW resets"));
    changed = saved;
    TruncateCopy(changed.hwRenderId, kEndpointIdLen, spk);  // default -> Speakers explicitly: still pending
    check("HW status: saved device not applied yet", has(HwStatusLines(&st, changed, &devs), HwStatusLevel::Warning, "DAW resets"));

    WHAMasterStats busy = st;
    busy.hwOpen = 0;
    busy.hwRenderId[0] = 0;
    busy.hwLastError = static_cast<int32_t>(0x8889000A);  // AUDCLNT_E_DEVICE_IN_USE
    busy.clockSource = CLOCK_INTERNAL;
    lines = HwStatusLines(&busy, saved, &devs);
    dump(lines);
    check("HW status: output busy is an error with the reason", has(lines, HwStatusLevel::Error, "in use by another application") &&
                                                                    has(lines, HwStatusLevel::Error, "0x8889000A"));
    check("HW status: internal Master Clock", has(lines, HwStatusLevel::Info, "internal"));

    WHAMasterStats unknown = st;
    TruncateCopy(unknown.hwRenderId, kStatsEndpointIdLen, "{gone}");
    check("HW status: unlisted device shows its ID", has(HwStatusLines(&unknown, saved, &devs), HwStatusLevel::Ok, "{gone}"));

    WHAMasterStats none{};
    none.sampleRate = 48000;
    none.asioBuffer = 128;
    none.hwRequestValid = 1;
    none.hwRequestedPeriod = 64;
    lines = HwStatusLines(&none, saved, &devs);
    check("HW status: no HW slot", has(lines, HwStatusLevel::Info, "Output: not open") && has(lines, HwStatusLevel::Info, "Input: not open"));

    WHAMasterStats glitchy = st;
    glitchy.hwUnderruns = 3;
    check("HW status: underruns warn", has(HwStatusLines(&glitchy, saved, &devs), HwStatusLevel::Warning, "3 underruns"));
  }

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
