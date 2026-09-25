#pragma once

// WinHookAudio config files — offline header-only, no file I/O.
// Vocabulary: Slot, Slot Table, Master Clock, Virtual Cable, Network Stream.
// The Control Panel's Save writes the Slot Table as two YAML files:
//   routes.yml   = the Master's INPUTS and OUTPUTS slots,
//   settings.yml = GENERAL (Master Clock, buffers, HW devices, Virtual Cables, Network Streams).
// Each file sets its whole part of the table: a key left out gets the default. Unknown keys are errors,
// so a typo is reported instead of ignored. The old one-file slots.json (WHASlotsJson.h) is still read.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>

#include "WHASlotTable.h"
#include "WHASlotsJson.h"
#include "WHAYaml.h"

namespace wha {

// ---- Words used instead of numbers ----

inline const char* SlotTypeWord(WHASlotType t) {
  static const char* const kWords[] = {"none", "hw", "virtual", "network", "bridge1", "bridge2", "bridge3", "bridge4"};
  return t <= SLOT_BRIDGE4 ? kWords[t] : "none";
}
inline bool ParseSlotType(std::string_view w, WHASlotType& out) {
  for (uint32_t t = SLOT_NONE; t <= SLOT_BRIDGE4; ++t)
    if (w == SlotTypeWord(static_cast<WHASlotType>(t))) {
      out = static_cast<WHASlotType>(t);
      return true;
    }
  return false;
}

// WHACableSetting::format (WHASampleKind numbers).
constexpr uint32_t kCableFormatCount = 5;
inline const char* CableFormatWord(uint32_t format) {
  static const char* const kWords[kCableFormatCount] = {"float32", "pcm16", "pcm24", "pcm32", "pcm24in32"};
  return format < kCableFormatCount ? kWords[format] : "float32";
}
inline bool ParseCableFormat(std::string_view w, uint8_t& out) {
  for (uint32_t f = 0; f < kCableFormatCount; ++f)
    if (w == CableFormatWord(f)) {
      out = static_cast<uint8_t>(f);
      return true;
    }
  return false;
}

// WHAHwMode.
inline const char* HwModeWord(uint32_t mode) {
  static const char* const kWords[kHwModeCount] = {"exclusive", "shared", "auto"};
  return mode < kHwModeCount ? kWords[mode] : "exclusive";
}
inline bool ParseHwMode(std::string_view w, uint8_t& out) {
  for (uint32_t m = 0; m < kHwModeCount; ++m)
    if (w == HwModeWord(m)) {
      out = static_cast<uint8_t>(m);
      return true;
    }
  return false;
}

inline const char* CodecWord(WHACodec c) {
  switch (c) {
    case WHA_PCM_I16: return "pcm-i16";
    case WHA_VORBIS: return "vorbis";
    default: return "pcm-f32";
  }
}
inline bool ParseCodec(std::string_view w, WHACodec& out) {
  for (WHACodec c : {WHA_PCM_F32, WHA_PCM_I16, WHA_VORBIS})
    if (w == CodecWord(c)) {
      out = c;
      return true;
    }
  return false;
}

// ---- Writing ----

inline std::string SerializeRoutes(const WHASlotTable& t) {
  std::string out =
      "# WinHookAudio routes: the Master's INPUTS and OUTPUTS slots, top to bottom = DAW channel order.\n"
      "# Written by the Control Panel's Save. Edit with the DAW closed; the Master reads it when it starts.\n"
      "#   type:       none | hw | virtual | network | bridge1 | bridge2 | bridge3 | bridge4\n"
      "#   srcChannel: hw 0 = L, 1 = R; virtual (cable - 1) * 8 + channel (0 = L, 1 = R, 2 = C ...);\n"
      "#               network / bridge: channel (0 = first)\n"
      "#   streamId:   hw device (0 = first in hwDevices), network stream (0..7)\n"
      "#   name:       \"- empty -\" = automatic name\n";
  out += "version: " + std::to_string(t.version) + "\n";
  for (int dir = 0; dir < 2; ++dir) {
    const WHASlot* slots = dir == 0 ? t.masterIn : t.masterOut;
    const uint32_t count = dir == 0 ? t.masterInCount : t.masterOutCount;
    out += dir == 0 ? "inputs:\n" : "outputs:\n";
    for (uint32_t i = 0; i < count && i < kMax; ++i) {
      const WHASlot& s = slots[i];
      out += "  - name: " + YamlQuote(std::string_view(s.name, strnlen(s.name, kNameLen))) + "\n";
      out += std::string("    type: ") + SlotTypeWord(s.type) + "\n";
      out += "    srcChannel: " + std::to_string(s.srcChannel) + "\n";
      out += "    streamId: " + std::to_string(s.streamId) + "\n";
      out += std::string("    enabled: ") + (s.enabled ? "true" : "false") + "\n";
      out += std::string("    loopback: ") + (s.loopback ? "true" : "false") + "\n";
    }
  }
  return out;
}

// `standalone` = a settings.yml of its own (header and version); false = the part after the routes in
// one exported file (SerializeEverything).
inline std::string SerializeSettings(const WHASlotTable& t, bool standalone = true) {
  const WHAGeneral& g = t.general;
  auto u = [](uint32_t v) { return std::to_string(v); };
  std::string out;
  if (standalone) {
    out += "# WinHookAudio settings (Control Panel GENERAL and NETWORK).\n"
           "# Written by the Control Panel's Save. Edit with the DAW closed; the Master reads it when it starts.\n";
    out += "version: " + u(t.version) + "\n";
  } else {
    out += "\n# ---- Settings (Control Panel GENERAL and NETWORK) ----\n";
  }
  out += "clock:                  # Master Clock: 44100 | 48000 | 96000 Hz, buffer 64..1024\n";
  out += "  sampleRate: " + u(g.sampleRate) + "\n";
  out += "  bitDepth: " + u(g.bitDepth) + "\n";
  out += "  asioBuffer: " + u(g.asioBuffer) + "\n";
  out += "buffers:                # samples; jitter in ms\n";
  out += "  hw: " + u(g.hwBuffer) + "                # 0 = Auto (the device minimum)\n";
  out += "  virtual: " + u(g.virtualBuffer) + "\n";
  out += "  bridge: [" + u(g.bridgeBuffer[0]) + ", " + u(g.bridgeBuffer[1]) + ", " + u(g.bridgeBuffer[2]) + ", " +
         u(g.bridgeBuffer[3]) + "]\n";
  out += "  networkPcm: " + u(g.networkPcmBuffer) + "\n";
  out += "  networkVorbis: " + u(g.networkVorbisBuffer) + "\n";
  out += "  jitterPcm: " + u(g.jitterPcm) + "\n";
  out += "  jitterVorbis: " + u(g.jitterVorbis) + "\n";
  out += "hwDevices:              # Windows endpoint IDs, device 1, 2, ...; \"\" = device 1: Windows default, others: none\n";
  out += "                        # modes, same order: exclusive (lowest latency) | shared (Windows mixer) | auto\n";
  for (int dir = 0; dir < 2; ++dir) {
    int count = 1;  // up to the last listed device: positions are device numbers
    for (int d = 1; d < kHwDevices; ++d)
      if (HwDeviceListed(t, dir == 1, d)) count = d + 1;
    out += dir == 0 ? "  outputs: [" : "  inputs: [";
    for (int d = 0; d < count; ++d) {
      if (d) out += ", ";
      const char* id = HwDeviceId(t, dir == 1, d);
      out += YamlQuote(std::string_view(id, strnlen(id, kEndpointIdLen)));
    }
    out += "]\n";
    out += dir == 0 ? "  outputModes: [" : "  inputModes: [";
    for (int d = 0; d < count; ++d) {
      if (d) out += ", ";
      out += HwModeWord(HwDeviceMode(t, dir == 1, d));
    }
    out += "]\n";
  }
  out += "virtualCables:\n";
  out += "  name: " + YamlQuote(std::string_view(g.virtualName, strnlen(g.virtualName, sizeof(g.virtualName)))) + "\n";
  out += "  cables:               # cable 1..8; channels 2 | 4 | 6 | 8; format float32 | pcm16 | pcm24 | pcm32 | pcm24in32\n";
  for (int c = 0; c < kVirtualSlotCables; ++c) {
    out += "    - channels: " + u(t.cables[c].channels) + "\n";
    out += std::string("      format: ") + CableFormatWord(t.cables[c].format) + "\n";
  }
  out += "network:                # stream 1..8 each way; codec pcm-f32 | pcm-i16 | vorbis\n";
  for (int dir = 0; dir < 2; ++dir) {
    out += dir == 0 ? "  tx:\n" : "  rx:\n";
    for (uint32_t i = 0; i < kNetStreams; ++i) {
      const WHANetworkStream& n = dir == 0 ? t.netTx[i] : t.netRx[i];
      char quality[32];
      std::snprintf(quality, sizeof(quality), "%.6g", n.quality);
      out += "    - ip: " + YamlQuote(std::string_view(n.ip, strnlen(n.ip, sizeof(n.ip)))) + "\n";
      out += "      port: " + u(n.port) + "\n";
      out += std::string("      codec: ") + CodecWord(n.codec) + "\n";
      out += std::string("      quality: ") + quality + "\n";
      out += "      channels: " + u(n.channels) + "\n";
    }
  }
  return out;
}

// ---- Reading ----

namespace config_detail {

inline bool Fail(std::string* error, const YamlNode& at, const std::string& where, const std::string& msg) {
  if (error) *error = "line " + std::to_string(at.line) + ": " + where + (where.empty() ? "" : ": ") + msg;
  return false;
}

inline bool CheckMap(const YamlNode& n, const std::string& where, std::initializer_list<const char*> allowed,
                     std::string* error) {
  if (!n.IsMap()) return Fail(error, n, where, "expected keys (key: value)");
  for (size_t i = 0; i < n.keys.size(); ++i) {
    bool known = false;
    for (const char* a : allowed) known = known || n.keys[i] == a;
    if (!known) return Fail(error, n.items[i], where, "unknown key \"" + n.keys[i] + "\"");
  }
  return true;
}

inline bool CheckList(const YamlNode& n, const std::string& where, size_t minCount, size_t maxCount, std::string* error) {
  if (!n.IsSeq()) return Fail(error, n, where, "expected a list");
  if (n.items.size() < minCount || n.items.size() > maxCount) {
    std::string range = minCount == maxCount ? std::to_string(minCount)
                                             : std::to_string(minCount) + ".." + std::to_string(maxCount);
    return Fail(error, n, where, "expected " + range + " items, found " + std::to_string(n.items.size()));
  }
  return true;
}

inline bool ReadLong(const YamlNode& n, const std::string& where, long long lo, long long hi, long long& out,
                     std::string* error) {
  if (!n.IsScalar() || n.scalar.empty()) return Fail(error, n, where, "expected a number");
  char* end = nullptr;
  const long long v = std::strtoll(n.scalar.c_str(), &end, 10);
  if (!end || *end != '\0') return Fail(error, n, where, "\"" + n.scalar + "\" is not a whole number");
  if (v < lo || v > hi) return Fail(error, n, where, n.scalar + " is out of range");
  out = v;
  return true;
}

inline bool ReadU32(const YamlNode* n, const std::string& where, uint32_t& out, std::string* error) {
  if (!n) return true;  // left out: keeps the default
  long long v = 0;
  if (!ReadLong(*n, where, 0, 0xFFFFFFFFll, v, error)) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

inline bool ReadI32(const YamlNode* n, const std::string& where, int32_t& out, std::string* error) {
  if (!n) return true;
  long long v = 0;
  if (!ReadLong(*n, where, INT32_MIN, INT32_MAX, v, error)) return false;
  out = static_cast<int32_t>(v);
  return true;
}

inline bool ReadBool(const YamlNode* n, const std::string& where, uint8_t& out, std::string* error) {
  if (!n) return true;
  if (n->IsScalar() && !n->quoted && n->scalar == "true") out = 1;
  else if (n->IsScalar() && !n->quoted && n->scalar == "false") out = 0;
  else return Fail(error, *n, where, "expected true or false");
  return true;
}

inline bool ReadFloat(const YamlNode* n, const std::string& where, float& out, std::string* error) {
  if (!n) return true;
  if (!n->IsScalar() || n->scalar.empty()) return Fail(error, *n, where, "expected a number");
  char* end = nullptr;
  const float v = std::strtof(n->scalar.c_str(), &end);
  if (!end || *end != '\0') return Fail(error, *n, where, "\"" + n->scalar + "\" is not a number");
  out = v;
  return true;
}

// A string into a fixed char array (terminator included in `size`).
inline bool ReadText(const YamlNode* n, const std::string& where, char* dst, size_t size, std::string* error) {
  if (!n) return true;
  if (!n->IsScalar()) return Fail(error, *n, where, "expected text");
  if (n->scalar.size() >= size)
    return Fail(error, *n, where, "longer than " + std::to_string(size - 1) + " characters");
  TruncateCopy(dst, size, n->scalar.c_str());
  return true;
}

template <class Word, class Parse>
inline bool ReadWord(const YamlNode* n, const std::string& where, const char* choices, Parse parse, Word& out,
                     std::string* error) {
  if (!n) return true;
  if (!n->IsScalar() || !parse(n->scalar, out))
    return Fail(error, *n, where, "\"" + n->scalar + "\" is not one of: " + choices);
  return true;
}

inline void TakeVersion(const YamlNode& root, WHASlotTable& t, std::string* error, bool& ok) {
  uint32_t v = 0;
  if (!ReadU32(root.Find("version"), "version", v, error)) ok = false;
  else if (v > t.version) t.version = v;
}

inline bool ReadSlot(const YamlNode& n, const std::string& where, WHASlot& s, std::string* error) {
  if (!CheckMap(n, where, {"name", "type", "srcChannel", "streamId", "enabled", "loopback"}, error)) return false;
  s = WHASlot{};
  s.enabled = 1;
  const YamlNode* type = n.Find("type");
  if (!type) return Fail(error, n, where, "missing \"type\"");
  if (!ReadWord(type, where + ": type", "none, hw, virtual, network, bridge1..bridge4", ParseSlotType, s.type, error))
    return false;
  if (!ReadText(n.Find("name"), where + ": name", s.name, kNameLen, error)) return false;
  if (!n.Find("name")) SetSlotName(s, "- empty -");
  return ReadI32(n.Find("srcChannel"), where + ": srcChannel", s.srcChannel, error) &&
         ReadI32(n.Find("streamId"), where + ": streamId", s.streamId, error) &&
         ReadBool(n.Find("enabled"), where + ": enabled", s.enabled, error) &&
         ReadBool(n.Find("loopback"), where + ": loopback", s.loopback, error);
}

inline bool ReadRoutes(const YamlNode& root, WHASlotTable& t, std::string* error) {
  if (!CheckMap(root, "", {"version", "inputs", "outputs"}, error)) return false;
  bool ok = true;
  TakeVersion(root, t, error, ok);
  if (!ok) return false;
  for (int dir = 0; dir < 2; ++dir) {
    const char* key = dir == 0 ? "inputs" : "outputs";
    const YamlNode* list = root.Find(key);
    if (!list) return Fail(error, root, "", std::string("missing \"") + key + "\"");
    if (!CheckList(*list, key, 1, kMax, error)) return false;
    WHASlot* slots = dir == 0 ? t.masterIn : t.masterOut;
    for (uint32_t i = 0; i < kMax; ++i) slots[i] = WHASlot{};
    const uint32_t count = static_cast<uint32_t>(list->items.size());
    for (uint32_t i = 0; i < count; ++i)
      if (!ReadSlot(list->items[i], std::string(dir == 0 ? "input " : "output ") + std::to_string(i + 1), slots[i], error))
        return false;
    (dir == 0 ? t.masterInCount : t.masterOutCount) = count;
  }
  return true;
}

inline bool ReadNetworkStream(const YamlNode& n, const std::string& where, WHANetworkStream& s, std::string* error) {
  if (!CheckMap(n, where, {"ip", "port", "codec", "quality", "channels"}, error)) return false;
  s = WHANetworkStream{};
  uint32_t port = s.port;
  if (!ReadText(n.Find("ip"), where + ": ip", s.ip, sizeof(s.ip), error) ||
      !ReadU32(n.Find("port"), where + ": port", port, error) ||
      !ReadWord(n.Find("codec"), where + ": codec", "pcm-f32, pcm-i16, vorbis", ParseCodec, s.codec, error) ||
      !ReadFloat(n.Find("quality"), where + ": quality", s.quality, error) ||
      !ReadU32(n.Find("channels"), where + ": channels", s.channels, error))
    return false;
  if (port > 65535) return Fail(error, *n.Find("port"), where + ": port", "out of range 1..65535");
  s.port = static_cast<uint16_t>(port);
  return true;
}

inline bool ReadSettings(const YamlNode& root, WHASlotTable& t, std::string* error) {
  if (!CheckMap(root, "", {"version", "clock", "buffers", "hwDevices", "virtualCables", "network"}, error)) return false;
  bool ok = true;
  TakeVersion(root, t, error, ok);
  if (!ok) return false;
  t.general = WHAGeneral{};
  ClearHwMoreDevices(t);
  t.hwModes = WHAHwModes{};
  for (WHACableSetting& c : t.cables) c = WHACableSetting{};
  for (uint32_t i = 0; i < kNetStreams; ++i) t.netTx[i] = t.netRx[i] = WHANetworkStream{};
  WHAGeneral& g = t.general;

  if (const YamlNode* clock = root.Find("clock")) {
    if (!CheckMap(*clock, "clock", {"sampleRate", "bitDepth", "asioBuffer"}, error) ||
        !ReadU32(clock->Find("sampleRate"), "clock: sampleRate", g.sampleRate, error) ||
        !ReadU32(clock->Find("bitDepth"), "clock: bitDepth", g.bitDepth, error) ||
        !ReadU32(clock->Find("asioBuffer"), "clock: asioBuffer", g.asioBuffer, error))
      return false;
  }
  if (const YamlNode* b = root.Find("buffers")) {
    if (!CheckMap(*b, "buffers",
                  {"hw", "virtual", "bridge", "networkPcm", "networkVorbis", "jitterPcm", "jitterVorbis"}, error) ||
        !ReadU32(b->Find("hw"), "buffers: hw", g.hwBuffer, error) ||
        !ReadU32(b->Find("virtual"), "buffers: virtual", g.virtualBuffer, error) ||
        !ReadU32(b->Find("networkPcm"), "buffers: networkPcm", g.networkPcmBuffer, error) ||
        !ReadU32(b->Find("networkVorbis"), "buffers: networkVorbis", g.networkVorbisBuffer, error) ||
        !ReadU32(b->Find("jitterPcm"), "buffers: jitterPcm", g.jitterPcm, error) ||
        !ReadU32(b->Find("jitterVorbis"), "buffers: jitterVorbis", g.jitterVorbis, error))
      return false;
    if (const YamlNode* bridge = b->Find("bridge")) {
      if (!CheckList(*bridge, "buffers: bridge", kBridgeCount, kBridgeCount, error)) return false;
      for (uint32_t i = 0; i < kBridgeCount; ++i)
        if (!ReadU32(&bridge->items[i], "buffers: bridge " + std::to_string(i + 1), g.bridgeBuffer[i], error))
          return false;
    }
  }
  if (const YamlNode* hw = root.Find("hwDevices")) {
    if (!CheckMap(*hw, "hwDevices", {"outputs", "inputs", "outputModes", "inputModes"}, error)) return false;
    for (int dir = 0; dir < 2; ++dir) {
      const char* key = dir == 0 ? "outputs" : "inputs";
      if (const YamlNode* list = hw->Find(key)) {
        const std::string where = std::string("hwDevices: ") + key;
        if (!CheckList(*list, where, 0, static_cast<size_t>(kHwDevices), error)) return false;
        for (size_t d = 0; d < list->items.size(); ++d)
          if (!ReadText(&list->items[d], where + " " + std::to_string(d + 1), HwDeviceId(t, dir == 1, static_cast<int>(d)),
                        kEndpointIdLen, error))
            return false;
      }
      const char* modesKey = dir == 0 ? "outputModes" : "inputModes";  // optional: absent = exclusive
      if (const YamlNode* modes = hw->Find(modesKey)) {
        const std::string where = std::string("hwDevices: ") + modesKey;
        if (!CheckList(*modes, where, 0, static_cast<size_t>(kHwDevices), error)) return false;
        uint8_t* place = dir == 0 ? t.hwModes.render : t.hwModes.capture;
        for (size_t d = 0; d < modes->items.size(); ++d)
          if (!ReadWord(&modes->items[d], where + " " + std::to_string(d + 1), "exclusive, shared, auto", ParseHwMode,
                        place[d], error))
            return false;
      }
    }
  }
  if (const YamlNode* vc = root.Find("virtualCables")) {
    if (!CheckMap(*vc, "virtualCables", {"name", "cables"}, error) ||
        !ReadText(vc->Find("name"), "virtualCables: name", g.virtualName, sizeof(g.virtualName), error))
      return false;
    if (const YamlNode* cables = vc->Find("cables")) {
      if (!CheckList(*cables, "virtualCables: cables", 0, static_cast<size_t>(kVirtualSlotCables), error)) return false;
      for (size_t c = 0; c < cables->items.size(); ++c) {
        const YamlNode& n = cables->items[c];
        const std::string where = "cable " + std::to_string(c + 1);
        uint32_t channels = t.cables[c].channels;
        if (!CheckMap(n, where, {"channels", "format"}, error) ||
            !ReadU32(n.Find("channels"), where + ": channels", channels, error) ||
            !ReadWord(n.Find("format"), where + ": format", "float32, pcm16, pcm24, pcm32, pcm24in32", ParseCableFormat,
                      t.cables[c].format, error))
          return false;
        if (channels > 255) return Fail(error, *n.Find("channels"), where + ": channels", "must be 2, 4, 6 or 8");
        t.cables[c].channels = static_cast<uint8_t>(channels);
      }
    }
  }
  if (const YamlNode* net = root.Find("network")) {
    if (!CheckMap(*net, "network", {"tx", "rx"}, error)) return false;
    for (int dir = 0; dir < 2; ++dir) {
      const char* key = dir == 0 ? "tx" : "rx";
      const YamlNode* list = net->Find(key);
      if (!list) continue;
      if (!CheckList(*list, std::string("network: ") + key, 0, kNetStreams, error)) return false;
      for (size_t i = 0; i < list->items.size(); ++i)
        if (!ReadNetworkStream(list->items[i], std::string(key) + " " + std::to_string(i + 1),
                               dir == 0 ? t.netTx[i] : t.netRx[i], error))
          return false;
    }
  }
  return true;
}

}  // namespace config_detail

// Each fills only its part of `t` (a key left out = its default) and may leave `t` half-written on
// failure: pass a scratch copy. Neither validates the whole table; ValidateSlots does.
inline bool DeserializeRoutes(std::string_view text, WHASlotTable& t, std::string* error) {
  YamlNode root;
  return ParseYaml(text, root, error) && config_detail::ReadRoutes(root, t, error);
}
inline bool DeserializeSettings(std::string_view text, WHASlotTable& t, std::string* error) {
  YamlNode root;
  return ParseYaml(text, root, error) && config_detail::ReadSettings(root, t, error);
}

// What an imported file held.
enum class ConfigKind { Routes, Settings, Everything };
inline const char* ConfigKindWord(ConfigKind k) {
  return k == ConfigKind::Routes ? "routes" : k == ConfigKind::Settings ? "settings" : "routes and settings";
}

// Routes and settings in one file (Export > Everything): the routes.yml keys, then the settings.yml keys.
inline std::string SerializeEverything(const WHASlotTable& t) { return SerializeRoutes(t) + SerializeSettings(t, false); }

// One config file of any kind into `t`: routes.yml replaces the slots, settings.yml GENERAL, an
// Everything file or an old slots.json both. Detected by content. The result must validate. `t` may be
// half-written on failure.
inline bool DeserializeConfigText(std::string_view text, WHASlotTable& t, ConfigKind* kind, std::string* error) {
  size_t p = 0;
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") p = 3;
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
  if (p < text.size() && text[p] == '{') {  // an old slots.json
    if (kind) *kind = ConfigKind::Everything;
    return DeserializeSlots(text.substr(p), t, error);
  }
  YamlNode root;
  if (!ParseYaml(text, root, error)) return false;
  if (!root.IsMap()) {
    if (error) *error = "not a WinHookAudio config file";
    return false;
  }
  const bool routes = root.Find("inputs") || root.Find("outputs");
  const bool settings = root.Find("clock") || root.Find("buffers") || root.Find("hwDevices") ||
                        root.Find("virtualCables") || root.Find("network");
  if (!routes && !settings) {
    if (error) *error = "not a WinHookAudio routes or settings file";
    return false;
  }
  if (routes && settings) {  // an Everything file: the routes keys to one reader, the rest to the other
    YamlNode routesPart, settingsPart;
    routesPart.kind = settingsPart.kind = YamlNode::Kind::Map;
    routesPart.line = settingsPart.line = root.line;
    for (size_t i = 0; i < root.keys.size(); ++i) {
      const std::string& key = root.keys[i];
      const bool isRoutes = key == "inputs" || key == "outputs";
      for (YamlNode* part : {&routesPart, &settingsPart})
        if (key == "version" || (part == &routesPart) == isRoutes) {
          part->keys.push_back(key);
          part->items.push_back(root.items[i]);
        }
    }
    if (kind) *kind = ConfigKind::Everything;
    if (!config_detail::ReadRoutes(routesPart, t, error) || !config_detail::ReadSettings(settingsPart, t, error)) return false;
    return ValidateSlots(t, error);
  }
  if (kind) *kind = routes ? ConfigKind::Routes : ConfigKind::Settings;
  if (!(routes ? config_detail::ReadRoutes(root, t, error) : config_detail::ReadSettings(root, t, error))) return false;
  return ValidateSlots(t, error);
}

}  // namespace wha
