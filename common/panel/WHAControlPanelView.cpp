#include "WHAControlPanelView.h"
#include "virtual/WHACableFormat.h"
#include "WHAVersion.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "imgui.h"

thread_local ImGuiContext* WhaImGuiTls = nullptr;

namespace wha {

namespace {

std::atomic<int> gAssertCount{0};

constexpr const char* kFilterNames = "All\0HW\0VIRTUAL\0NETWORK\0BRIDGE1\0BRIDGE2\0BRIDGE3\0BRIDGE4\0";
constexpr const char* kCodecNames = "PCM_F32\0PCM_I16\0VORBIS\0";

constexpr uint32_t kRates[] = {44100, 48000, 96000};
constexpr uint32_t kFrames[] = {64, 128, 256, 512, 1024};
constexpr uint32_t kHwFrames[] = {0, 64, 128, 256, 512, 1024};  // 0 = Auto (device minimum)
constexpr uint32_t kJitterPcmMs[] = {10, 20, 40, 80};
constexpr uint32_t kJitterVorbisMs[] = {50, 80, 120, 200};

// One set of model operations per list, so INPUTS and OUTPUTS share the row code
// while keeping independent indices (12).
struct SlotListOps {
  bool isInput;
  const char* label;
  const char* prefix;
  const char* dragType;
  const char* addLabel;
  bool (*add)(PanelModel&);
  bool (*insertAbove)(PanelModel&, uint32_t);
  bool (*insertBelow)(PanelModel&, uint32_t);
  bool (*duplicate)(PanelModel&, uint32_t);
  bool (*remove)(PanelModel&, uint32_t);
  bool (*move)(PanelModel&, uint32_t, uint32_t);
  bool (*setLoopback)(PanelModel&, uint32_t, bool);
  bool (*setEnabled)(PanelModel&, uint32_t, bool);
  bool (*setName)(PanelModel&, uint32_t, const char*);
  bool (*setType)(PanelModel&, uint32_t, WHASlotType);
  bool (*setSource)(PanelModel&, uint32_t, int32_t, int32_t);
};

const SlotListOps kInputOps{true, "INPUTS", "In", "WHA_IN", "+Add Input", AddInput, InsertEmptyAbove, InsertEmptyBelow,
                            DuplicateInput, DeleteInput, MoveInput, SetInputLoopback, SetInputEnabled, SetInputName,
                            SetInputType, SetInputSource};
const SlotListOps kOutputOps{false, "OUTPUTS", "Out", "WHA_OUT", "+Add Output", AddOutput, InsertEmptyAboveOutput,
                             InsertEmptyBelowOutput, DuplicateOutput, DeleteOutput, MoveOutput, SetOutputLoopback,
                             SetOutputEnabled, SetOutputName, SetOutputType, SetOutputSource};

// The Source cell: one menu picks what a slot carries (type and source together), with HW devices
// by name, so nobody has to know that "HW" + "L" means the left channel of the GENERAL device.
void DrawSourceCell(PanelModel& edit, bool isInput, uint32_t idx, const PanelDevices* devices, const char* const* hwNames,
                    WHABridgeShared* const* bridges, bool cableDriver) {
  WHASlot& s = (isInput ? edit.table.masterIn : edit.table.masterOut)[idx];
  const std::string preview = SlotSourceLabel(s, isInput, hwNames);
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (!ImGui::BeginCombo("##source", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
    if (s.type == SLOT_HW && ImGui::IsItemHovered()) {
      const char* name = hwNames ? hwNames[HwDeviceOf(s)] : nullptr;
      ImGui::SetTooltip("%s device #%d: %s", isInput ? "From" : "To", HwDeviceOf(s) + 1,
                        name ? name : "the Windows default device");
    }
    return;
  }
  if (ImGui::Selectable("- empty -", s.type == SLOT_NONE)) AssignSource(edit, isInput, idx, SLOT_NONE, 0, 0);

  if (ImGui::BeginMenu("Hardware", !IsGeneralReadOnly(edit))) {
    ImGui::TextDisabled("Each device offers all its channels (its format in Windows Sound settings).");
    if (!isInput) ImGui::TextDisabled("Output #1 is the clock: the others are resampled to it.");
    const std::vector<PanelEndpoint>* list = devices ? (isInput ? &devices->capture : &devices->render) : nullptr;
    auto channelsOf = [&](const char* id) {  // 2 when the device is not connected or not known
      if (list)
        for (const PanelEndpoint& e : *list)
          if (e.id == id) return e.channels;
      return 2;
    };
    auto deviceMenu = [&](const char* id, std::string label, int channels) {
      ImGui::PushID(id[0] ? id : "default");
      const int listed = FindHwDevice(edit.table, isInput, id);
      if (channels != 2) label += "  (" + std::to_string(channels) + " ch)";
      if (listed >= 0) label += "  (#" + std::to_string(listed + 1) + ")";
      const bool isCurrent = s.type == SLOT_HW && listed == HwDeviceOf(s);
      if (ImGui::BeginMenu(label.c_str(), CanAssignHw(edit, isInput, idx, id))) {
        for (int ch = 0; ch < channels; ++ch) {
          char item[24];
          if (channels == 2) std::snprintf(item, sizeof(item), "%s", ch == 0 ? "L (left)" : "R (right)");
          else if (ch < 2) std::snprintf(item, sizeof(item), "Ch %d  %s", ch + 1, ch == 0 ? "(L)" : "(R)");
          else std::snprintf(item, sizeof(item), "Ch %d", ch + 1);
          if (ImGui::MenuItem(item, nullptr, isCurrent && s.srcChannel == ch)) AssignHw(edit, isInput, idx, id, ch);
        }
        ImGui::EndMenu();
      }
      ImGui::PopID();
    };
    const std::string defId = devices ? (isInput ? devices->defaultCaptureId : devices->defaultRenderId) : std::string();
    const char* defName = list ? EndpointName(*list, defId.c_str()) : nullptr;
    deviceMenu("", defName ? std::string("Windows default (") + defName + ")" : std::string("Windows default"),
               list ? channelsOf(defId.c_str()) : 2);
    if (list)
      for (const PanelEndpoint& e : *list) deviceMenu(e.id.c_str(), e.name, e.channels);
    for (int d = 0; d < kHwDevices; ++d) {  // a listed device not connected: keep it visible
      const char* id = HwDeviceId(edit.table, isInput, d);
      if (HwDeviceListed(edit.table, isInput, d) && id[0] && (!list || !EndpointName(*list, id)))
        deviceMenu(id, std::string("not connected: ") + id, 2);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Virtual Cable")) {
    ImGui::TextDisabled(isInput ? "Windows apps play into these (they appear as sound outputs)."
                                : "Windows apps record from these (they appear as microphones).");
    if (!cableDriver) ImGui::TextDisabled("Cable driver not running: only DAW OUT -> DAW IN loops work.");
    ImGui::TextDisabled("Channels per cable: GENERAL > Virtual Cables.");
    for (int c = 0; c < kVirtualSlotCables; ++c) {
      const unsigned channels = IsValidCableSetting(edit.table.cables[c]) ? edit.table.cables[c].channels : 2u;
      ImGui::PushID(c);
      const std::string label = VirtualCableLabel(edit.table.general, c) + "  (" + CableChannelsLabel(channels) + ")";
      if (ImGui::BeginMenu(label.c_str())) {
        for (unsigned ch = 0; ch < channels; ++ch) {
          char item[24];
          std::snprintf(item, sizeof(item), "Ch %u  %s", ch + 1, CableChannelName(channels, ch));
          if (ImGui::MenuItem(item, nullptr, s.type == SLOT_VIRTUAL && VirtualCableOf(s) == c && VirtualChannelOf(s) == static_cast<int>(ch)))
            AssignSource(edit, isInput, idx, SLOT_VIRTUAL, c * kVirtualCableChannels + static_cast<int>(ch), 0);
        }
        ImGui::EndMenu();
      }
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu(isInput ? "Network (receive)" : "Network (send)")) {
    ImGui::TextDisabled(isInput ? "Audio from another PC. Set streams up in the NETWORK tab."
                                : "Audio to another PC. Set streams up in the NETWORK tab.");
    for (int st = 0; st < static_cast<int>(kNetStreams); ++st) {
      const WHANetworkStream& ns = isInput ? edit.table.netRx[st] : edit.table.netTx[st];
      const int channels = ns.channels ? static_cast<int>(ns.channels) : 2;
      ImGui::PushID(st);
      if (ImGui::BeginMenu(NetworkStreamLabel(ns, isInput, st).c_str())) {
        for (int ch = 0; ch < channels; ++ch) {
          char item[16];
          std::snprintf(item, sizeof(item), "Ch %d", ch + 1);
          if (ImGui::MenuItem(item, nullptr, s.type == SLOT_NETWORK && s.streamId == st && s.srcChannel == ch))
            AssignSource(edit, isInput, idx, SLOT_NETWORK, ch, st);
        }
        ImGui::EndMenu();
      }
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Bridge")) {
    ImGui::TextDisabled(isInput ? "Audio from other ASIO apps that pick \"WinHookAudio Bridge N\"."
                                : "Audio to other ASIO apps that pick \"WinHookAudio Bridge N\".");
    ImGui::TextDisabled("Ch N is channel N in that app. Each app gets at least 2 channels.");
    if (!isInput) ImGui::TextDisabled("A greyed channel is already used by another OUTPUTS row.");
    for (int b = 0; b < 4; ++b) {
      const auto type = static_cast<WHASlotType>(SLOT_BRIDGE1 + b);
      auto channelItem = [&](int ch) {
        char item[16];
        std::snprintf(item, sizeof(item), "Ch %d", ch + 1);
        if (ImGui::MenuItem(item, nullptr, s.type == type && s.srcChannel == ch, CanAssignBridge(edit, isInput, idx, type, ch)))
          AssignSource(edit, isInput, idx, type, ch, 0);
      };
      ImGui::PushID(b);
      if (ImGui::BeginMenu(BridgeLabel(b, bridges ? bridges[b] : nullptr).c_str())) {
        constexpr int kFirstPage = 16;  // Ch 1-16 directly, the rest in pages of 16
        for (int ch = 0; ch < kFirstPage; ++ch) channelItem(ch);
        for (int page = kFirstPage; page < static_cast<int>(kBridgeChannels); page += kFirstPage) {
          char label[24];
          std::snprintf(label, sizeof(label), "Ch %d-%d", page + 1, page + kFirstPage);
          if (ImGui::BeginMenu(label)) {
            for (int ch = page; ch < page + kFirstPage; ++ch) channelItem(ch);
            ImGui::EndMenu();
          }
        }
        ImGui::EndMenu();
      }
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
  ImGui::EndCombo();
}

// Structural edits are deferred until the clipper loop ends.
struct PendingOp {
  enum Kind { None, Add, InsertAbove, InsertBelow, Duplicate, Remove, Move } kind = None;
  uint32_t a = 0;
  uint32_t b = 0;
};

bool ComboU32(const char* label, uint32_t& value, const uint32_t* options, size_t count, const char* fmt) {
  char preview[32];
  std::snprintf(preview, sizeof(preview), fmt, value);
  bool changed = false;
  if (ImGui::BeginCombo(label, preview)) {
    for (size_t i = 0; i < count; ++i) {
      char item[32];
      std::snprintf(item, sizeof(item), fmt, options[i]);
      if (ImGui::Selectable(item, options[i] == value)) {
        changed = options[i] != value;
        value = options[i];
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// HW period: "Auto" (0 = the device's minimum) or a frame count.
bool BeginComboHwFrames(uint32_t& value) {
  char preview[32];
  if (value) std::snprintf(preview, sizeof(preview), "%u", value); else std::snprintf(preview, sizeof(preview), "Auto");
  bool changed = false;
  if (ImGui::BeginCombo("Hardware (KS Exclusive)", preview)) {
    for (uint32_t option : kHwFrames) {
      char item[32];
      if (option) std::snprintf(item, sizeof(item), "%u", option); else std::snprintf(item, sizeof(item), "Auto (device minimum)");
      if (ImGui::Selectable(item, option == value)) {
        changed = option != value;
        value = option;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

void DrawSlotList(PanelModel& edit, const SlotListOps& ops, PanelFilter& filter, char* search, size_t searchLen,
                  int& rowsDrawn, int bridgeIndex, const AboutInfo& about, const PanelDevices* devices,
                  WHABridgeShared* const* bridges) {
  WHASlotTable& t = edit.table;
  const uint32_t count = ops.isInput ? t.masterInCount : t.masterOutCount;
  WHASlot* slots = ops.isInput ? t.masterIn : t.masterOut;
  const bool locked = bridgeIndex >= 0;  // filter already forced by DrawControlPanel

  PendingOp op;
  ImGui::Text("%s: [%u/%u]", ops.label, count, kMax);
  for (const std::string& b : about.bridgeClients) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", b.c_str());
  }
  int f = static_cast<int>(filter);
  ImGui::SetNextItemWidth(110);
  ImGui::BeginDisabled(locked);
  if (ImGui::Combo("Filter", &f, kFilterNames)) filter = static_cast<PanelFilter>(f);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(200);
  ImGui::InputTextWithHint("##search", "Search name", search, searchLen);
  ImGui::SameLine();
  ImGui::BeginDisabled(count >= kMax);
  if (ImGui::Button(ops.addLabel)) op.kind = PendingOp::Add;
  ImGui::EndDisabled();

  const char* hwNames[kHwDevices];
  HwDeviceNames(devices, t, ops.isInput, hwNames);

  std::vector<uint32_t> visible;
  visible.reserve(count);
  const std::string searchText(search);
  for (uint32_t i = 0; i < count; ++i)
    if (MatchesFilter(slots[i], filter) && MatchesSearch(slots[i], searchText)) visible.push_back(i);

  rowsDrawn = 0;
  constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_SizingFixedFit;
  if (ImGui::BeginTable("slots", 7, kTableFlags, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("::", ImGuiTableColumnFlags_WidthFixed, 22);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 250);
    ImGui::TableSetupColumn("Name in DAW", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("En", ImGuiTableColumnFlags_WidthFixed, 32);
    ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 28);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const uint32_t idx = visible[static_cast<size_t>(row)];
        WHASlot& s = slots[idx];
        ++rowsDrawn;
        ImGui::PushID(static_cast<int>(idx));
        ImGui::TableNextRow();

        // :: drag handle spanning the row; drag = memmove within this list only
        ImGui::TableSetColumnIndex(0);
        ImGui::Selectable("::", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
        if (ImGui::BeginDragDropSource()) {
          ImGui::SetDragDropPayload(ops.dragType, &idx, sizeof(idx));
          ImGui::Text("Move %s%02u", ops.prefix, idx + 1);
          ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(ops.dragType)) {
            uint32_t from = 0;
            std::memcpy(&from, p->Data, sizeof(from));
            op = {PendingOp::Move, from, idx};
          }
          ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("row")) {
          const bool full = count >= kMax;
          if (ImGui::MenuItem("Insert Empty Above", nullptr, false, !full)) op = {PendingOp::InsertAbove, idx, 0};
          if (ImGui::MenuItem("Insert Empty Below", nullptr, false, !full)) op = {PendingOp::InsertBelow, idx, 0};
          if (ImGui::MenuItem("Duplicate", nullptr, false, !full)) op = {PendingOp::Duplicate, idx, 0};
          if (ImGui::MenuItem("Delete", nullptr, false, count > 1)) op = {PendingOp::Remove, idx, 0};
          ImGui::EndPopup();
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s%02u", ops.prefix, idx + 1);

        ImGui::TableSetColumnIndex(2);
        DrawSourceCell(edit, ops.isInput, idx, devices, hwNames, bridges, about.sysRunning);

        ImGui::TableSetColumnIndex(3);
        char name[kNameLen];
        std::memcpy(name, s.name, sizeof(name));
        name[kNameLen - 1] = '\0';
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (s.type != SLOT_NONE && IsPlaceholderName(name)) {
          // Nobody named it: the DAW gets the automatic name, shown greyed; typing sets a name,
          // clearing it goes back to automatic.
          char autoName[kNameLen];
          AutoSlotName(slots, count, idx, ops.isInput, hwNames, autoName);
          name[0] = '\0';
          if (ImGui::InputTextWithHint("##name", autoName, name, sizeof(name))) ops.setName(edit, idx, name);
        } else if (ImGui::InputText("##name", name, sizeof(name))) {
          ops.setName(edit, idx, name);
        }

        ImGui::TableSetColumnIndex(4);
        bool loop = s.loopback != 0;
        ImGui::BeginDisabled(!IsLoopbackEditable(s));
        if (ImGui::Checkbox("##loop", &loop)) ops.setLoopback(edit, idx, loop);
        ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(5);
        bool en = s.enabled != 0;
        if (ImGui::Checkbox("##en", &en)) ops.setEnabled(edit, idx, en);

        ImGui::TableSetColumnIndex(6);
        ImGui::BeginDisabled(count <= 1);
        if (ImGui::SmallButton("x")) op = {PendingOp::Remove, idx, 0};
        ImGui::EndDisabled();

        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }

  switch (op.kind) {
    case PendingOp::Add: ops.add(edit); break;
    case PendingOp::InsertAbove: ops.insertAbove(edit, op.a); break;
    case PendingOp::InsertBelow: ops.insertBelow(edit, op.a); break;
    case PendingOp::Duplicate: ops.duplicate(edit, op.a); break;
    case PendingOp::Remove: ops.remove(edit, op.a); break;
    case PendingOp::Move: ops.move(edit, op.a, op.b); break;
    case PendingOp::None: break;
  }
}

void DrawNetworkStreams(PanelModel& edit, bool tx, std::string& status) {
  const char* id = tx ? "tx" : "rx";
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable(id, tx ? 7 : 8, kFlags)) return;
  ImGui::TableSetupColumn(tx ? "Tx" : "Rx", ImGuiTableColumnFlags_WidthFixed, 36);
  ImGui::TableSetupColumn(tx ? "Destination IP" : "Peer IP", ImGuiTableColumnFlags_WidthFixed, 150);
  ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 90);
  ImGui::TableSetupColumn("Codec", ImGuiTableColumnFlags_WidthFixed, 110);
  ImGui::TableSetupColumn("Q", ImGuiTableColumnFlags_WidthFixed, 110);
  ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 90);
  ImGui::TableSetupColumn("BW", ImGuiTableColumnFlags_WidthFixed, 110);
  if (!tx) ImGui::TableSetupColumn("Linked INPUTS", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();

  for (uint32_t i = 0; i < kNetStreams; ++i) {
    WHANetworkStream s = tx ? GetNetworkTx(edit, i) : GetNetworkRx(edit, i);
    bool changed = false;
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%s%u", tx ? "Tx" : "Rx", i + 1);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::InputTextWithHint("##ip", "192.168.1.50", s.ip, sizeof(s.ip), ImGuiInputTextFlags_CharsDecimal);
    ImGui::TableSetColumnIndex(2);
    int port = s.port;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputInt("##port", &port, 0, 0)) {
      s.port = static_cast<uint16_t>(port < 1 ? 1 : (port > 65535 ? 65535 : port));
      changed = true;
    }
    ImGui::TableSetColumnIndex(3);
    int codec = static_cast<int>(s.codec);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##codec", &codec, kCodecNames)) {
      s.codec = static_cast<WHACodec>(codec);
      const uint32_t maxCh = s.codec == WHA_VORBIS ? kMaxVorbisChannels : kMaxPcmChannels;
      if (s.channels > maxCh) s.channels = maxCh;
      changed = true;
    }
    ImGui::TableSetColumnIndex(4);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(s.codec != WHA_VORBIS);
    float q = s.quality;  // WHANetworkStream is packed: never take &s.quality
    if (ImGui::SliderFloat("##q", &q, 0.1f, 1.0f, "Q%.1f", ImGuiSliderFlags_AlwaysClamp)) {
      s.quality = q;
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::TableSetColumnIndex(5);
    int ch = static_cast<int>(s.channels);
    const int maxCh = static_cast<int>(s.codec == WHA_VORBIS ? kMaxVorbisChannels : kMaxPcmChannels);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragInt("##ch", &ch, 0.2f, 1, maxCh, "Ch %d", ImGuiSliderFlags_AlwaysClamp)) {
      s.channels = static_cast<uint32_t>(ch);
      changed = true;
    }
    ImGui::TableSetColumnIndex(6);
    const double mbps = NetworkBandwidthMbps(s);
    if (mbps < 1.0) ImGui::Text("%.0f kbps", mbps * 1000.0);
    else ImGui::Text("%.1f Mbps", mbps);
    if (!tx) {
      ImGui::TableSetColumnIndex(7);
      int linked = 0;
      for (uint32_t j = 0; j < edit.table.masterInCount; ++j) {
        const WHASlot& in = edit.table.masterIn[j];
        if (in.type == SLOT_NETWORK && in.streamId == static_cast<int32_t>(i)) ++linked;
      }
      if (linked) ImGui::Text("%d input(s)", linked);
      else ImGui::TextDisabled("none");
    }
    if (changed && !(tx ? SetNetworkTx(edit, i, s) : SetNetworkRx(edit, i, s))) {
      char msg[48];
      std::snprintf(msg, sizeof(msg), "%s%u: rejected", tx ? "Tx" : "Rx", i + 1);
      status = msg;
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// One HW device choice: "Windows default" (device 0 only) or an endpoint. A saved ID that is not
// present shows as not connected (the Worker will fail to open it, not fall back silently).
bool ComboEndpoint(const char* label, const char* currentId, const std::vector<PanelEndpoint>* list, std::string& chosen,
                   bool allowDefault = true) {
  const char* name = !*currentId ? "Windows default" : list ? EndpointName(*list, currentId) : nullptr;
  std::string preview = name ? name : std::string("(not connected) ") + currentId;
  bool changed = false;
  if (ImGui::BeginCombo(label, preview.c_str())) {
    if (allowDefault && ImGui::Selectable("Windows default", !*currentId)) {
      changed = *currentId != 0;
      chosen.clear();
    }
    for (size_t i = 0; list && i < list->size(); ++i) {
      const PanelEndpoint& e = (*list)[i];
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(e.name.c_str(), e.id == currentId)) {
        changed = e.id != currentId;
        chosen = e.id;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Each cable's format: channels and bits; the rate is the Master Clock's. Applied live on Save.
void DrawCableFormats(PanelModel& edit) {
  constexpr uint32_t kChannelChoices[] = {2, 4, 6, 8};
  constexpr uint32_t kFormatOrder[] = {1, 2, 4, 3, 0};  // 16, 24, 24 in 32, 32, 32 float
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("cables", 4, kFlags)) return;
  ImGui::TableSetupColumn("Cable", ImGuiTableColumnFlags_WidthFixed, 200);
  ImGui::TableSetupColumn("Channels", ImGuiTableColumnFlags_WidthFixed, 130);
  ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthFixed, 150);
  ImGui::TableSetupColumn("Rate", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();
  for (int c = 0; c < kVirtualSlotCables; ++c) {
    const WHACableSetting cs = edit.table.cables[c];
    ImGui::PushID(c);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(VirtualCableLabel(edit.table.general, c).c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##ch", CableChannelsLabel(cs.channels))) {
      for (uint32_t n : kChannelChoices) {
        char item[24];
        std::snprintf(item, sizeof(item), "%s (%u ch)", CableChannelsLabel(n), n);
        if (ImGui::Selectable(item, cs.channels == n)) SetCableChannels(edit, c, n);
      }
      ImGui::EndCombo();
    }
    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##bits", CableFormatLabel(cs.format))) {
      for (uint32_t f : kFormatOrder)
        if (ImGui::Selectable(CableFormatLabel(f), cs.format == f)) SetCableFormat(edit, c, f);
      ImGui::EndCombo();
    }
    ImGui::TableSetColumnIndex(3);
    ImGui::TextDisabled("%u Hz (Master)", edit.table.general.sampleRate);
    ImGui::PopID();
  }
  ImGui::EndTable();
  ImGui::TextDisabled("Windows gets exactly this format; it switches when you Save (no DAW reset).");
}

void DrawGeneral(PanelModel& edit, const PanelViewState& state, PanelViewResult& result) {
  WHAGeneral& g = edit.table.general;
  const bool readOnly = IsGeneralReadOnly(edit);
  if (readOnly) ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.3f, 1), "Follows Master %u/32/%u (read-only in Bridge)", g.sampleRate, g.asioBuffer);
  ImGui::BeginDisabled(readOnly);

  ImGui::SeparatorText("1. MASTER CLOCK");
  uint32_t rate = g.sampleRate;
  uint32_t buf = g.asioBuffer;
  ImGui::SetNextItemWidth(140);
  if (ComboU32("Sample Rate", rate, kRates, std::size(kRates), "%u")) SetMasterClock(edit, rate, g.asioBuffer);
  ImGui::SetNextItemWidth(140);
  ImGui::LabelText("Bit Depth", "32 Float");
  ImGui::SetNextItemWidth(140);
  if (ComboU32("ASIO Buffer", buf, kFrames, std::size(kFrames), "%u")) SetMasterClock(edit, g.sampleRate, buf);
  ImGui::SameLine();
  ImGui::TextDisabled("%.1f ms @ %u/%u", 1000.0 * g.asioBuffer / g.sampleRate, g.asioBuffer, g.sampleRate / 1000);
  ImGui::TextDisabled("Changing the Master Clock asks the DAW to reset on Save.");

  ImGui::SeparatorText("2. HARDWARE DEVICES");
  for (int dir = 0; dir < 2; ++dir) {
    const bool isInput = dir == 1;
    const std::vector<PanelEndpoint>* list = state.devices ? (isInput ? &state.devices->capture : &state.devices->render) : nullptr;
    ImGui::PushID(dir);
    ImGui::TextUnformatted(isInput ? "Inputs (HW IN slots)" : "Outputs (HW OUT slots)");
    int removeDevice = -1;
    for (int d = 0; d < kHwDevices; ++d) {
      if (!HwDeviceListed(edit.table, isInput, d)) continue;
      ImGui::PushID(d);
      ImGui::Text("  #%d", d + 1);
      ImGui::SameLine(60);
      std::string chosen;
      ImGui::SetNextItemWidth(360);
      if (ComboEndpoint("##device", HwDeviceId(edit.table, isInput, d), list, chosen, d == 0))
        SetHwDevice(edit, isInput, d, chosen.c_str());
      ImGui::SameLine();
      static const char* const kModeNames[kHwModeCount] = {"Exclusive", "Shared", "Auto"};
      const WHAHwMode mode = HwDeviceMode(edit.table, isInput, d);
      ImGui::SetNextItemWidth(100);
      if (ImGui::BeginCombo("##mode", kModeNames[mode])) {
        for (uint8_t m = 0; m < kHwModeCount; ++m)
          if (ImGui::Selectable(kModeNames[m], m == mode)) SetHwMode(edit, isInput, d, m);
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exclusive: lowest latency; no other app can use the device while the DAW streams.\n"
                          "Shared: through the Windows mixer; other apps keep their sound; more latency (see Actual).\n"
                          "Auto: Exclusive, or Shared when the device refuses exclusive mode (not allowed in\n"
                          "its Windows settings, or no exclusive format at this sample rate).");
      ImGui::SameLine();
      if (d > 0 && ImGui::SmallButton("x")) removeDevice = d;
      if (d > 0) ImGui::SameLine();
      const int used = HwDeviceSlotCount(edit.table, isInput, d);
      if (!isInput && d == 0) ImGui::TextDisabled("clock; %d slot%s", used, used == 1 ? "" : "s");
      else if (used) ImGui::TextDisabled("%d slot%s", used, used == 1 ? "" : "s");
      else ImGui::TextDisabled("no slot uses it: not opened");
      ImGui::PopID();
    }
    if (removeDevice > 0) RemoveHwDevice(edit, isInput, removeDevice);
    bool full = true;
    for (int d = 1; d < kHwDevices; ++d) full = full && HwDeviceListed(edit.table, isInput, d);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 52);
    ImGui::SetNextItemWidth(360);
    ImGui::BeginDisabled(full || !list);
    if (ImGui::BeginCombo("##add", isInput ? "+ Add input device" : "+ Add output device")) {
      for (size_t i = 0; list && i < list->size(); ++i) {
        const PanelEndpoint& e = (*list)[i];
        if (FindHwDevice(edit.table, isInput, e.id.c_str()) >= 0) continue;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(e.name.c_str())) AddHwDevice(edit, isInput, e.id.c_str());
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::TextDisabled("Each device: all its channels. Output #1 paces the DAW; other devices are resampled to it.");
  if (ImGui::SmallButton("Refresh devices")) result.refreshDevices = true;
  ImGui::SameLine();
  ImGui::TextUnformatted("Windows:");
  ImGui::SameLine();
  if (ImGui::SmallButton("Playback devices")) result.openWindowsSound = kSoundPlayback;
  ImGui::SameLine();
  if (ImGui::SmallButton("Recording devices")) result.openWindowsSound = kSoundRecording;
  ImGui::SameLine();
  if (ImGui::SmallButton("Sound settings")) result.openWindowsSound = kSoundSettingsApp;
  ImGui::SameLine();
  ImGui::TextDisabled("Mode: Exclusive = lowest latency, the device is the DAW's alone; Shared = other apps keep sound. "
                      "Changing asks the DAW to reset on Save.");
  if (state.hwStatus) {
    ImGui::TextUnformatted("Actual:");
    for (const HwStatusLine& line : *state.hwStatus) {
      const ImVec4 color = line.level == HwStatusLevel::Error     ? ImVec4(1.0f, 0.42f, 0.42f, 1.0f)
                           : line.level == HwStatusLevel::Warning ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f)
                           : line.level == HwStatusLevel::Ok      ? ImVec4(0.5f, 0.85f, 0.55f, 1.0f)
                                                                  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
      ImGui::PushStyleColor(ImGuiCol_Text, color);
      ImGui::Bullet();
      ImGui::TextWrapped("%s", line.text.c_str());
      ImGui::PopStyleColor();
    }
  }

  ImGui::SeparatorText("3. PER-THING WORKER BUFFERS");
  uint32_t v = g.hwBuffer;
  ImGui::SetNextItemWidth(140);
  if (BeginComboHwFrames(v)) SetHwBuffer(edit, v);
  v = g.virtualBuffer;
  ImGui::SetNextItemWidth(140);
  if (ComboU32("Virtual Cable", v, kFrames, std::size(kFrames), "%u")) SetVirtualBuffer(edit, v);
  for (int b = 0; b < static_cast<int>(kBridgeCount); ++b) {
    char label[16];
    std::snprintf(label, sizeof(label), "Bridge%d", b + 1);
    v = g.bridgeBuffer[b];
    ImGui::SetNextItemWidth(100);
    if (ComboU32(label, v, kFrames, std::size(kFrames), "%u")) SetBridgeBuffer(edit, b, v);
    if (b + 1 < static_cast<int>(kBridgeCount)) ImGui::SameLine();
  }
  v = g.networkPcmBuffer;
  ImGui::SetNextItemWidth(100);
  if (ComboU32("Network PCM", v, kFrames, std::size(kFrames), "%u")) SetNetworkPcmBuffer(edit, v);
  ImGui::SameLine();
  v = g.networkVorbisBuffer;
  ImGui::SetNextItemWidth(100);
  if (ComboU32("Vorbis", v, kFrames, std::size(kFrames), "%u")) SetNetworkVorbisBuffer(edit, v);
  v = g.jitterPcm;
  ImGui::SetNextItemWidth(100);
  if (ComboU32("Jitter PCM", v, kJitterPcmMs, std::size(kJitterPcmMs), "%u ms")) SetJitterPcm(edit, v);
  ImGui::SameLine();
  v = g.jitterVorbis;
  ImGui::SetNextItemWidth(100);
  if (ComboU32("Jitter Vorbis", v, kJitterVorbisMs, std::size(kJitterVorbisMs), "%u ms")) SetJitterVorbis(edit, v);

  ImGui::SeparatorText("4. VIRTUAL CABLES");
  char vname[sizeof(g.virtualName)];
  std::memcpy(vname, g.virtualName, sizeof(vname));
  vname[sizeof(vname) - 1] = '\0';
  ImGui::SetNextItemWidth(260);
  if (ImGui::InputText("Name", vname, sizeof(vname))) SetVirtualCableName(edit, vname);
  DrawCableFormats(edit);

  ImGui::EndDisabled();
}

void DrawAbout(PanelModel& edit, const AboutInfo& info, PanelViewResult& result) {
  ImGui::Text("WinHookAudio %s", info.version.c_str());
  ImGui::Text("Made by %s", WHA_AUTHOR);
  ImGui::TextDisabled("%s - %s", WHA_COPYRIGHT, WHA_LICENSE);
  ImGui::TextDisabled("%s", WHA_URL);
  ImGui::Separator();
  ImGui::Text("WinHookAudio.sys: %s", info.sysStatus.c_str());
  ImGui::Text("ASIO CLSIDs: %d (Master + Bridge1..4)", info.clsidCount);
  ImGui::TextWrapped("Saved to: %s", info.configPath.c_str());
  ImGui::SeparatorText("Bridge clients");
  for (const std::string& b : info.bridgeClients) ImGui::BulletText("%s", b.c_str());
  ImGui::Separator();
  if (ImGui::Button("Export...")) ImGui::OpenPopup("##export");
  if (ImGui::BeginPopup("##export")) {
    if (ImGui::MenuItem("Routes (routes.yml)")) result.exportRoutes = true;
    if (ImGui::MenuItem("Settings (settings.yml)")) result.exportSettings = true;
    if (ImGui::MenuItem("Everything (winhookaudio.yml)")) result.exportEverything = true;
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Import...")) result.importSlots = true;
  ImGui::SameLine();
  ImGui::BeginDisabled(!edit.isMaster);
  if (ImGui::Button("Reset Default")) ResetToDefault(edit);
  ImGui::EndDisabled();
}

}  // namespace

void ImGuiAssertFailed(const char* expr, const char* file, int line) {
  gAssertCount.fetch_add(1, std::memory_order_relaxed);
  char msg[512];
  std::snprintf(msg, sizeof(msg), "WinHookAudio ImGui assert: %s (%s:%d)\n", expr, file, line);
  OutputDebugStringA(msg);
  std::fputs(msg, stderr);
}

int ImGuiAssertCount() { return gAssertCount.load(std::memory_order_relaxed); }

void ApplyPanelStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.FrameRounding = 3.0f;
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0x1E / 255.0f, 0x1E / 255.0f, 0x1E / 255.0f, 1.0f);
  style.Colors[ImGuiCol_ChildBg] = style.Colors[ImGuiCol_WindowBg];
}

namespace {

// One direction of the Bridge popup: "Out 1  ->  Master IN 4  Drums L", unrouted channels greyed.
void DrawBridgeRoutes(const WHASlotTable& table, int bridgeIndex, bool appOutputs) {
  const std::vector<BridgeRoute> routes = BridgeRoutes(table, bridgeIndex, appOutputs);
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable(appOutputs ? "outs" : "ins", 3, kFlags)) return;
  ImGui::TableSetupColumn("This app", ImGuiTableColumnFlags_WidthFixed, 90);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30);
  ImGui::TableSetupColumn(appOutputs ? "Master input" : "Master output");
  ImGui::TableHeadersRow();
  int lastChannel = -1;
  for (const BridgeRoute& r : routes) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (r.channel != lastChannel) ImGui::Text("%s %d", appOutputs ? "Out" : "In", r.channel + 1);
    lastChannel = r.channel;
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(appOutputs ? "->" : "<-");
    ImGui::TableNextColumn();
    if (r.masterSlot < 0) ImGui::TextDisabled("not routed (silent)");
    else ImGui::Text("%s %d  %s", appOutputs ? "IN" : "OUT", r.masterSlot + 1, r.masterName.c_str());
  }
  ImGui::EndTable();
}

}  // namespace

PanelViewResult DrawBridgePanel(const WHASlotTable& table, int bridgeIndex, const WHABridgeShared* shared, bool masterOpen) {
  PanelViewResult result;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  constexpr ImGuiWindowFlags kRoot = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("WinHookAudio Bridge", nullptr, kRoot);

  ImGui::Text("WinHookAudio Bridge %d", bridgeIndex + 1);
  ImGui::Separator();
  ImGui::TextUnformatted("Master:");
  ImGui::SameLine();
  if (masterOpen) {
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "open");
    ImGui::SameLine();
    ImGui::Text("  %u Hz \xC2\xB7 %u samples", table.general.sampleRate, table.general.asioBuffer);
  } else {
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "not open - open WinHookAudio Master in your main DAW");
  }
  if (shared) {
    ImGui::Text("Apps on this Bridge: %d / %u", CountBridgeClients(*shared), kBridgeClients);
    // Blocks an app was too late for (the Master mixed silence) or skipped after falling far behind:
    // either one is a dropout. More than a few: a larger Bridge buffer in the Master's GENERAL.
    const int delay = BridgeDelayBlocks(table.general.bridgeBuffer[bridgeIndex], table.general.asioBuffer);
    ImGui::TextDisabled("Delay %d blocks (%u samples)", delay, delay * table.general.asioBuffer);
    for (uint32_t c = 0; c < kBridgeClients; ++c) {
      if (shared->owner[c] == 0) continue;
      const long long late = shared->clientLate[c], skipped = shared->clientSkipped[c];
      const ImVec4 color = late || skipped ? ImVec4(0.95f, 0.75f, 0.3f, 1) : ImVec4(0.4f, 0.85f, 0.4f, 1);
      ImGui::TextColored(color, "  App %u: %s, dropouts %lld late + %lld skipped", c + 1,
                         shared->clientBlocks[c] >= 0 ? "running" : "stopped", late, skipped);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Dropouts: blocks the app was too late for. If they keep growing,\n"
                        "raise this Bridge's buffer in the Master panel: GENERAL > 3. PER-THING WORKER BUFFERS.");
  }

  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
  ImGui::BeginChild("routes", ImVec2(0, -footer));
  ImGui::SeparatorText("TO MASTER (this app's outputs)");
  DrawBridgeRoutes(table, bridgeIndex, true);
  ImGui::SeparatorText("FROM MASTER (this app's inputs)");
  DrawBridgeRoutes(table, bridgeIndex, false);
  ImGui::Spacing();
  ImGui::TextDisabled("Channels are set in the Master panel: Source > Bridge > Bridge %d > Ch.", bridgeIndex + 1);
  ImGui::EndChild();

  ImGui::Separator();
  if (ImGui::Button("Close")) result.close = true;
  ImGui::End();
  return result;
}

PanelViewResult DrawControlPanel(PanelModel& edit, PanelViewState& state, WHABridgeShared* bridges[4]) {
  PanelViewResult result;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  constexpr ImGuiWindowFlags kRoot = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("WinHookAudio", nullptr, kRoot);

  if (state.bridgeIndex >= 0) {  // Bridge popup: both lists locked to BRIDGE(n), whatever tab is open
    const auto locked = static_cast<PanelFilter>(static_cast<uint32_t>(PanelFilter::Bridge1) + state.bridgeIndex);
    state.inFilter = locked;
    state.outFilter = locked;
  }
  const AboutInfo about = GetAboutInfo(edit, bridges);
  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
  state.rowsDrawnIn = 0;
  state.rowsDrawnOut = 0;
  state.activeTab = -1;
  const int requested = state.requestTab;
  state.requestTab = -1;
  auto tabFlags = [requested](int tab) { return requested == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
  if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem("INPUTS 512", nullptr, tabFlags(kTabInputs))) {
      state.activeTab = kTabInputs;
      ImGui::BeginChild("body", ImVec2(0, -footer));
      DrawSlotList(edit, kInputOps, state.inFilter, state.inSearch, sizeof(state.inSearch), state.rowsDrawnIn,
                   state.bridgeIndex, about, state.devices, bridges);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("OUTPUTS 512", nullptr, tabFlags(kTabOutputs))) {
      state.activeTab = kTabOutputs;
      ImGui::BeginChild("body", ImVec2(0, -footer));
      DrawSlotList(edit, kOutputOps, state.outFilter, state.outSearch, sizeof(state.outSearch), state.rowsDrawnOut,
                   state.bridgeIndex, about, state.devices, bridges);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("NETWORK 8", nullptr, tabFlags(kTabNetwork))) {
      state.activeTab = kTabNetwork;
      ImGui::BeginChild("body", ImVec2(0, -footer));
      ImGui::SeparatorText("Tx (Master OUT -> UDP 6980)");
      DrawNetworkStreams(edit, true, state.status);
      ImGui::SeparatorText("Rx (UDP 6980 -> INPUTS Type=NETWORK)");
      DrawNetworkStreams(edit, false, state.status);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("GENERAL", nullptr, tabFlags(kTabGeneral))) {
      state.activeTab = kTabGeneral;
      ImGui::BeginChild("body", ImVec2(0, -footer));
      DrawGeneral(edit, state, result);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("ABOUT", nullptr, tabFlags(kTabAbout))) {
      state.activeTab = kTabAbout;
      ImGui::BeginChild("body", ImVec2(0, -footer));
      DrawAbout(edit, about, result);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::BeginDisabled(!state.dirty);
  if (ImGui::Button("Save")) result.save = true;
  ImGui::SameLine();
  if (ImGui::Button("Revert")) result.revert = true;
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Close")) result.close = true;
  ImGui::SameLine();
  if (state.dirty) ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.3f, 1), "* unsaved changes");
  if (!state.status.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.status.c_str());
  }
  ImGui::End();
  return result;
}

}  // namespace wha
