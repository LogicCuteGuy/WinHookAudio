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
    {
      PanelModel br = genModel;
      AddInput(br);
      check("Bridge1 Ch2 on IN 0", AssignSource(br, true, 0, SLOT_BRIDGE1, 1, 0));
      const WHASlotTable bridgeBefore = br.table;
      check("Bridge channel move Ch2 -> Ch4 is DAW-visible (reset)",
            AssignSource(br, true, 0, SLOT_BRIDGE1, 3, 0) && DawVisibleChanged(bridgeBefore, br.table));
    }
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

  // Automatic names: an assigned slot nobody named reaches the DAW as what it carries, not "- empty -".
  {
    // hwDevice: device 0's name; hwMore: devices 1..3 (nullptr: unknown).
    auto dawName = [](const PanelModel& m, uint32_t i, bool in, const char* hwDevice = nullptr,
                      const char* const* hwMore = nullptr) {
      const char* devices[kHwDevices] = {hwDevice, hwMore ? hwMore[0] : nullptr, hwMore ? hwMore[1] : nullptr,
                                         hwMore ? hwMore[2] : nullptr};
      char n[kNameLen];
      if (in) DawChannelName(m.table.masterIn, m.table.masterInCount, i, true, devices, n);
      else DawChannelName(m.table.masterOut, m.table.masterOutCount, i, false, devices, n);
      return std::string(n);
    };
    auto nmHeap = std::make_unique<PanelModel>();  // ~50 KB each: main's stack is nearly full of them
    PanelModel& nm = *nmHeap;
    nm.table.masterInCount = 1;
    nm.table.masterOutCount = 1;
    AddInput(nm);  // In02: "- empty -"
    AddInput(nm);  // In03
    AddOutput(nm);
    check("empty slot -> '- empty -' in the DAW", dawName(nm, 1, true) == "- empty -");
    SetInputType(nm, 1, SLOT_HW);
    check("assigned HW IN, unnamed -> 'HW In L'", dawName(nm, 1, true) == "HW In L");
    check("the stored name stays unset (automatic)", IsPlaceholderName(nm.table.masterIn[1].name));
    check("SetInputSource HW R", SetInputSource(nm, 1, 1, 0) && dawName(nm, 1, true) == "HW In R");
    check("SetInputSource HW channel 3 rejected (stereo device)", !SetInputSource(nm, 1, 2, 0) && nm.table.masterIn[1].srcChannel == 1);
    SetOutputType(nm, 1, SLOT_HW);
    check("assigned HW OUT -> 'HW Out L'", dawName(nm, 1, false) == "HW Out L");
    nm.table.masterIn[2].srcChannel = 5;  // left over from another type
    SetInputType(nm, 2, SLOT_HW);
    check("SetInputType HW resets a non-L/R source to L", nm.table.masterIn[2].srcChannel == 0 && dawName(nm, 2, true) == "HW In L");
    SetInputType(nm, 2, SLOT_VIRTUAL);
    check("VIRTUAL cable 4 R (source 7) -> 'Virtual 4 R'", SetInputSource(nm, 2, 7, 0) && dawName(nm, 2, true) == "Virtual 4 R");
    check("VIRTUAL source 16 rejected (8 stereo cables)", !SetInputSource(nm, 2, 16, 0));
    SetInputType(nm, 2, SLOT_NETWORK);
    check("NETWORK stream 2 ch 1 -> 'Rx3 Ch1'", SetInputSource(nm, 2, 0, 2) && dawName(nm, 2, true) == "Rx3 Ch1");
    check("NETWORK stream 8 rejected", !SetInputSource(nm, 2, 0, 8));
    check("AssignSource Bridge1 Ch3 and Ch1", AssignSource(nm, true, 0, SLOT_BRIDGE1, 2, 0) && AssignSource(nm, true, 2, SLOT_BRIDGE1, 0, 0));
    check("BRIDGE1 slots named by the channel they pick, not their order",
          dawName(nm, 0, true) == "Bridge1 Ch3" && dawName(nm, 2, true) == "Bridge1 Ch1");
    check("Bridge source label shows the channel", SlotSourceLabel(nm.table.masterIn[0], true, nullptr) == "Bridge1 Â· Ch3");
    check("Bridge channel 65 rejected", !AssignSource(nm, true, 0, SLOT_BRIDGE1, 64, 0) && nm.table.masterIn[0].srcChannel == 2);
    check("INPUTS rows may share a Bridge channel", CanAssignBridge(nm, true, 2, SLOT_BRIDGE1, 2));
    check("Bridge channel count: at least 2, up to the highest picked",
          BridgeChannelCount(nm.table.masterIn, nm.table.masterInCount, SLOT_BRIDGE1) == 3 &&
          BridgeChannelCount(nm.table.masterIn, nm.table.masterInCount, SLOT_BRIDGE2) == 2);
    check("FindBridgeSlot: channel -> row, unrouted -> -1",
          FindBridgeSlot(nm.table.masterIn, nm.table.masterInCount, SLOT_BRIDGE1, 2) == 0 &&
          FindBridgeSlot(nm.table.masterIn, nm.table.masterInCount, SLOT_BRIDGE1, 1) == -1);
    check("AssignSource Bridge2 Ch2 on an output", AssignSource(nm, false, 0, SLOT_BRIDGE2, 1, 0));
    check("an OUTPUTS Bridge channel is taken by one row",
          !CanAssignBridge(nm, false, 1, SLOT_BRIDGE2, 1) && CanAssignBridge(nm, false, 0, SLOT_BRIDGE2, 1) &&
          CanAssignBridge(nm, false, 1, SLOT_BRIDGE2, 0) && CanAssignBridge(nm, false, 1, SLOT_BRIDGE1, 1));
    {
      // Bridge popup lines: IN 1 and IN 3 on Bridge1 Ch3 (shared), OUT 2 on Bridge1 Ch1.
      WHASlotTable bt{};
      bt.masterInCount = 3;
      bt.masterOutCount = 2;
      bt.masterIn[0].type = SLOT_BRIDGE1; bt.masterIn[0].srcChannel = 2; SetSlotName(bt.masterIn[0], "Drums");
      bt.masterIn[2].type = SLOT_BRIDGE1; bt.masterIn[2].srcChannel = 2;
      bt.masterOut[1].type = SLOT_BRIDGE1; bt.masterOut[1].srcChannel = 0;
      bt.masterIn[1].type = SLOT_BRIDGE2; bt.masterIn[1].srcChannel = 5;  // another Bridge: not listed
      const std::vector<BridgeRoute> outs = BridgeRoutes(bt, 0, true);
      check("Bridge popup outputs: Ch1-2 unrouted, Ch3 -> IN 1 'Drums' and IN 3",
            outs.size() == 4 && outs[0].channel == 0 && outs[0].masterSlot == -1 && outs[1].channel == 1 &&
                outs[1].masterSlot == -1 && outs[2].channel == 2 && outs[2].masterSlot == 0 && outs[2].masterName == "Drums" &&
                outs[3].channel == 2 && outs[3].masterSlot == 2 && outs[3].masterName == "Bridge1 Ch3");
      const std::vector<BridgeRoute> ins = BridgeRoutes(bt, 0, false);
      check("Bridge popup inputs: Ch1 <- OUT 2, Ch2 unrouted",
            ins.size() == 2 && ins[0].masterSlot == 1 && ins[0].masterName == "Bridge1 Ch1" && ins[1].masterSlot == -1);
      check("Bridge popup: bad Bridge index lists nothing", BridgeRoutes(bt, 4, true).empty());
    }
    SetOutputType(nm, 0, SLOT_NONE);
    SetInputName(nm, 1, "Vocal Mic");
    check("a name the user typed is kept", dawName(nm, 1, true) == "Vocal Mic");
    SetInputSource(nm, 1, 0, 0);
    SetInputType(nm, 1, SLOT_VIRTUAL);
    check("...through source and type changes", dawName(nm, 1, true) == "Vocal Mic");
    SetInputName(nm, 1, "");
    check("clearing the name goes back to automatic", dawName(nm, 1, true) == "Virtual 1 L");
    SetInputType(nm, 1, SLOT_NONE);
    check("back to NONE -> '- empty -'", dawName(nm, 1, true) == "- empty -");

    // With the device's name: "Microphone L", short enough for ASIO's 32 characters.
    SetInputType(nm, 1, SLOT_HW);
    check("HW name carries the device's short name",
          dawName(nm, 1, true, "Microphone (High Definition Audio Device)") == "Microphone L");
    const std::string longName = dawName(nm, 1, true, "An Extremely Long Audio Interface Product Name 18i20");
    check("long device name still fits with its side", longName.size() < kNameLen && longName.back() == 'L');
    char shortName[kNameLen];
    ShortDeviceName("CABLE Output (VB-Audio Virtual Cable)", shortName, sizeof(shortName));
    check("ShortDeviceName drops the driver part", std::string(shortName) == "CABLE Output");

    // Source menu: one pick sets type + source; HW also picks the GENERAL device of that direction.
    const char* mic = "{0.0.1.00000000}.{aaaaaaaa-0000-0000-0000-000000000002}";
    const char* usb = "{0.0.1.00000000}.{aaaaaaaa-0000-0000-0000-000000000003}";
    PanelDevices devs;
    devs.capture.push_back({mic, "Microphone (Realtek Audio)"});
    devs.capture.push_back({usb, "Line In (USB Interface)"});
    devs.defaultCaptureId = mic;
    check("HwDeviceName: empty ID = the Windows default's name",
          HwDeviceName(&devs, nm.table.general, true) && std::string(HwDeviceName(&devs, nm.table.general, true)) == "Microphone (Realtek Audio)");
    // In02 already uses device #1 (the Windows default): the USB interface is added as device #2.
    check("AssignHw: USB interface R, added as device #2", AssignHw(nm, true, 2, usb, 1) && nm.table.masterIn[2].type == SLOT_HW &&
                                                               nm.table.masterIn[2].srcChannel == 1 && nm.table.masterIn[2].streamId == 1 &&
                                                               nm.table.general.hwCaptureId[0] == 0 &&
                                                               std::strcmp(nm.table.hwMore.captureId[0], usb) == 0);
    const char* names[kHwDevices];
    HwDeviceNames(&devs, nm.table, true, names);
    check("HwDeviceNames: #1 the default, #2 the USB interface", names[0] && std::string(names[0]) == "Microphone (Realtek Audio)" &&
                                                                     names[1] && std::string(names[1]) == "Line In (USB Interface)" && !names[2]);
    check("...and the DAW names follow each slot's device",
          dawName(nm, 2, true, names[0], names + 1) == "Line In R" && dawName(nm, 1, true, names[0], names + 1) == "Microphone L");
    check("Source label names the device", SlotSourceLabel(nm.table.masterIn[2], true, names) == "Line In \xC2\xB7 R");
    check("unknown device #2 -> 'HW In 2 R'", dawName(nm, 2, true) == "HW In 2 R");
    check("AssignHw a listed device reuses it", AssignHw(nm, true, 1, usb, 0) && nm.table.masterIn[1].streamId == 1 &&
                                                    FindHwDevice(nm.table, true, usb) == 1 && HwDeviceSlotCount(nm.table, true, 1) == 2);
    check("device #1 now unused: picking another device switches #1 instead of adding",
          AssignHw(nm, true, 1, mic, 1) && nm.table.masterIn[1].streamId == 0 && std::strcmp(nm.table.general.hwCaptureId, mic) == 0);
    check("a device cannot be listed twice", !SetHwDevice(nm, true, 2, usb) && AddHwDevice(nm, true, usb) < 0);
    check("an unlisted device index is rejected as a source", !SetInputSource(nm, 1, 0, 3) && !AssignSource(nm, true, 1, SLOT_HW, 0, 3));
    {
      auto full = std::make_unique<PanelModel>(nm);
      const char* ids[] = {"{x2}", "{x3}"};
      check("four input devices at most", AddHwDevice(*full, true, ids[0]) == 2 && AddHwDevice(*full, true, ids[1]) == 3 &&
                                              AddHwDevice(*full, true, "{x4}") < 0);
      check("...then a new device cannot be assigned while #1 is in use",
            !CanAssignHw(*full, true, 2, "{x4}") && !AssignHw(*full, true, 2, "{x4}", 0) && full->table.masterIn[2].streamId == 1);
      check("RemoveHwDevice empties its slots", RemoveHwDevice(*full, true, 1) && full->table.masterIn[2].type == SLOT_NONE &&
                                                    full->table.hwMore.captureId[0][0] == 0 && !RemoveHwDevice(*full, true, 0));
      check("the list round-trips through slots.json", [&] {
        WHASlotTable back{};
        std::string err;
        const bool ok = DeserializeSlots(SerializeSlots(full->table), back, &err);
        if (!ok) std::printf("  JSON error: %s\n", err.c_str());
        return ok && std::strcmp(back.hwMore.captureId[1], "{x2}") == 0 && std::strcmp(back.hwMore.captureId[2], "{x3}") == 0 &&
               back.hwMore.captureId[0][0] == 0 && back.masterIn[1].streamId == 0;
      }());
      check("a device list change is DAW-visible (reset)", DawVisibleChanged(nm.table, full->table));
    }
    check("AssignHw side 2 rejected", !AssignHw(nm, true, 2, usb, 2));
    check("AssignHw from a Bridge popup rejected (GENERAL is read-only there)", [&] {
      auto bridgeHeap = std::make_unique<PanelModel>(nm);
      bridgeHeap->isMaster = false;
      return !AssignHw(*bridgeHeap, true, 2, mic, 0);
    }());
    check("AssignSource Virtual 3 L", AssignSource(nm, true, 2, SLOT_VIRTUAL, 4, 0) && nm.table.masterIn[2].type == SLOT_VIRTUAL &&
                                          SlotSourceLabel(nm.table.masterIn[2], true, nullptr) == "Virtual 3 \xC2\xB7 L");
    check("Virtual Cable menu uses the GENERAL name", VirtualCableLabel(nm.table.general, 0) == "WinHookAudio Virtual 1");
    WHANetworkStream rx{};
    check("Network stream not set up says where", NetworkStreamLabel(rx, true, 0) == "Rx1  (not set up: NETWORK tab)");
    TruncateCopy(rx.ip, sizeof(rx.ip), "192.168.1.50");
    check("Network stream shows peer, codec, channels",
          NetworkStreamLabel(rx, true, 2) == "Rx3  from 192.168.1.50:6980 \xC2\xB7 PCM 32 \xC2\xB7 2 ch");
    auto shared = std::make_unique<WHABridgeShared>();
    check("Bridge label: no app", BridgeLabel(0, shared.get()) == "Bridge 1  (no app connected)");
    shared->owner[0] = 100;
    shared->owner[2] = 200;
    check("Bridge label: 2 apps", BridgeLabel(1, shared.get()) == "Bridge 2  (2 apps connected)");
    check("Bridge label: unknown", BridgeLabel(3, nullptr) == "Bridge 4");
    check("AssignSource Tx2 Ch2 on an output", AssignSource(nm, false, 1, SLOT_NETWORK, 1, 1) &&
                                                   SlotSourceLabel(nm.table.masterOut[1], false, nullptr) == "Tx2 \xC2\xB7 Ch2");
    check("AssignSource invalid source leaves the slot", !AssignSource(nm, true, 2, SLOT_NETWORK, 0, 9) &&
                                                             nm.table.masterIn[2].type == SLOT_VIRTUAL);
    check("AssignSource empty", AssignSource(nm, true, 2, SLOT_NONE, 0, 0) && SlotSourceLabel(nm.table.masterIn[2], true, nullptr) == "- empty -");
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
    check("HW status: no input dropout line while clean", !has(lines, HwStatusLevel::Warning, "Input dropouts"));
    WHAMasterStats dropping = st;
    dropping.hwInStarved = 4;
    dropping.hwInGrowths = 4;
    dropping.hwInTrims = 1;
    dropping.hwInSkipped = 480;  // 10 ms at 48 kHz
    dropping.workerOverruns = 2;
    lines = HwStatusLines(&dropping, saved, &devs);
    dump(lines);
    check("HW status: input dropouts counted", has(lines, HwStatusLevel::Warning, "4 ran empty (buffer grew 4 times), 1 overflowed") &&
                                                   has(lines, HwStatusLevel::Warning, "10.0 ms skipped"));
    check("HW status: Worker late warns", has(lines, HwStatusLevel::Warning, "Worker late 2 times"));

    // More devices: each its own line (clock drift, latency), failures, a device listed twice.
    const char* cable = "{0.0.0.00000000}.{aaaaaaaa-0000-0000-0000-000000000009}";
    devs.render.push_back({cable, "CABLE Input"});
    WHAMasterStats more = st;
    WHAHwDeviceStats& o2 = more.hwMoreOut[0];
    TruncateCopy(o2.requestedId, kStatsEndpointIdLen, cable);
    TruncateCopy(o2.id, kStatsEndpointIdLen, cable);
    o2.used = o2.open = 1;
    o2.sameAs = -1;
    o2.period = 128;
    o2.format = HW_FORMAT_FLOAT32;
    o2.latency = 960;
    o2.driftPpmMilli = 42500;
    o2.driftEngaged = 1;
    WHAHwDeviceStats& o3 = more.hwMoreOut[1];
    o3.used = 1;
    o3.sameAs = 0;
    WHAHwDeviceStats& i2 = more.hwMoreIn[0];
    i2.used = 1;
    i2.sameAs = -1;
    i2.lastError = static_cast<int32_t>(0x8889000A);
    WHAHwMore savedMore{};
    TruncateCopy(savedMore.renderId[0], kEndpointIdLen, cable);
    lines = HwStatusLines(&more, saved, &devs, &savedMore);
    dump(lines);
    check("HW status: output #2 with its clock and latency", has(lines, HwStatusLevel::Ok, "Output #2: CABLE Input - exclusive float32") &&
                                                               has(lines, HwStatusLevel::Ok, "latency 20.0 ms, clock +42.5 ppm (resampling)"));
    check("HW status: output #3 is output #1 listed twice", has(lines, HwStatusLevel::Info, "Output #3: the same device as #1"));
    check("HW status: input #2 failed", has(lines, HwStatusLevel::Error, "Input #2: FAILED - in use"));
    check("HW status: more devices as saved: nothing pending", !has(lines, HwStatusLevel::Warning, "DAW resets"));
    WHAHwMore changedMore = savedMore;
    TruncateCopy(changedMore.captureId[2], kEndpointIdLen, mic);
    check("HW status: a device added since start is pending", has(HwStatusLines(&more, saved, &devs, &changedMore), HwStatusLevel::Warning, "DAW resets"));
    more.hwMoreOut[0].underruns = 2;
    more.hwMoreOut[0].gaps = 1;
    check("HW status: output #2 dropouts warn", has(HwStatusLines(&more, saved, &devs), HwStatusLevel::Warning, "device ran dry 2 times, 1 refills"));
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
