#pragma once

// WinHookAudio slots.json schema — offline header-only.
// Vocabulary: Slot, Slot Table, Master Clock, Loopback, Network Stream.
// No audio, no devices, no network, no file I/O here — pure string round-trip.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "WHASlotTable.h"

namespace wha {

// ---- Validation (offline, no I/O) ----

inline bool ValidateSlots(const WHASlotTable& table, std::string* error) {
  auto fail = [&](const char* msg) -> bool {
    if (error) *error = msg;
    return false;
  };
  if (!IsValidCount(table.masterInCount)) return fail("masterInCount out of range 1..512");
  if (!IsValidCount(table.masterOutCount)) return fail("masterOutCount out of range 1..512");
  for (uint32_t i = 0; i < table.masterInCount; ++i) {
    const auto& s = table.masterIn[i];
    if (s.type > SLOT_BRIDGE4) return fail("masterIn slot type invalid");
    if (s.type == SLOT_NETWORK && (s.streamId < 0 || s.streamId >= WHA_NET_STREAMS))
      return fail("masterIn network streamId out of range");
    if (!IsValidSlotEnabled(s.enabled)) return fail("masterIn enabled must be 0/1");
    if (!IsValidSlotLoopback(s.loopback)) return fail("masterIn loopback must be 0/1");
    if (!IsLoopbackValid(s)) return fail("masterIn loopback only valid on Virtual");
    if (std::strlen(s.name) >= WHA_NAME_LEN) return fail("masterIn name not null-terminated");
  }
  for (uint32_t i = 0; i < table.masterOutCount; ++i) {
    const auto& s = table.masterOut[i];
    if (s.type > SLOT_BRIDGE4) return fail("masterOut slot type invalid");
    if (s.type == SLOT_NETWORK && (s.streamId < 0 || s.streamId >= WHA_NET_STREAMS))
      return fail("masterOut network streamId out of range");
    if (!IsValidSlotEnabled(s.enabled)) return fail("masterOut enabled must be 0/1");
    if (!IsValidSlotLoopback(s.loopback)) return fail("masterOut loopback must be 0/1");
    if (!IsLoopbackValid(s)) return fail("masterOut loopback only valid on Virtual");
    if (std::strlen(s.name) >= WHA_NAME_LEN) return fail("masterOut name not null-terminated");
  }
  if (!IsValidMasterClock(table.general.sampleRate, table.general.asioBuffer))
    return fail("general Master Clock invalid");
  if (table.general.bitDepth != 32) return fail("general bitDepth must be 32");
  if (table.general.virtualCables != 8 && table.general.virtualCables != 64)
    return fail("general virtualCables must be 8 or 64");
  if (std::strlen(table.general.virtualName) >= 32) return fail("general virtualName too long");
  if (strnlen(table.general.hwRenderId, kEndpointIdLen) >= kEndpointIdLen) return fail("general hwRenderId too long");
  if (strnlen(table.general.hwCaptureId, kEndpointIdLen) >= kEndpointIdLen) return fail("general hwCaptureId too long");
  for (int d = 1; d < kHwDevices; ++d)
    if (strnlen(HwDeviceId(table, false, d), kEndpointIdLen) >= kEndpointIdLen ||
        strnlen(HwDeviceId(table, true, d), kEndpointIdLen) >= kEndpointIdLen)
      return fail("general more HW device id too long");
  for (int d = 0; d < kHwDevices; ++d)
    if (table.hwModes.render[d] >= kHwModeCount || table.hwModes.capture[d] >= kHwModeCount)
      return fail("general HW device mode must be 0..2");
  for (int c = 0; c < kVirtualSlotCables; ++c)
    if (!IsValidCableSetting(CableSetting(table, c))) return fail("general cables: channels 2/4/6/8, format 0..4");
  for (int i = 0; i < WHA_BRIDGE_COUNT; ++i) {
    uint32_t v = table.general.bridgeBuffer[i];
    if (v != 64 && v != 128 && v != 256 && v != 512 && v != 1024)
      return fail("general bridgeBuffer invalid");
  }
  for (int i = 0; i < WHA_NET_STREAMS; ++i) {
    const auto& tx = table.netTx[i];
    const auto& rx = table.netRx[i];
    if (!IsValidCodec(tx.codec)) return fail("netTx codec invalid");
    if (!IsValidCodec(rx.codec)) return fail("netRx codec invalid");
    if (!IsValidNetworkChannels(tx.codec, tx.channels)) return fail("netTx channels invalid");
    if (!IsValidNetworkChannels(rx.codec, rx.channels)) return fail("netRx channels invalid");
    if (tx.quality < 0.1f || tx.quality > 1.0f) return fail("netTx quality out of range");
    if (rx.quality < 0.1f || rx.quality > 1.0f) return fail("netRx quality out of range");
    if (tx.port == 0) return fail("netTx port invalid");
    if (rx.port == 0) return fail("netRx port invalid");
    if (std::strlen(tx.ip) >= sizeof(tx.ip)) return fail("netTx ip too long");
    if (std::strlen(rx.ip) >= sizeof(rx.ip)) return fail("netRx ip too long");
  }
  return true;
}

inline void IncrementVersion(WHASlotTable& table) { ++table.version; }

// ---- JSON helpers (minimal, offline) ----

namespace detail {

inline std::string EscapeJsonString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[7];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
  return out;
}

inline std::string SlotToJson(const WHASlot& s) {
  std::string out = "{";
  out += "\"type\":" + std::to_string(static_cast<uint32_t>(s.type)) + ",";
  out += "\"srcChannel\":" + std::to_string(s.srcChannel) + ",";
  out += "\"streamId\":" + std::to_string(s.streamId) + ",";
  out += "\"name\":" + EscapeJsonString(s.name) + ",";
  out += "\"enabled\":" + std::string(s.enabled ? "true" : "false") + ",";
  out += "\"loopback\":" + std::string(s.loopback ? "true" : "false");
  out += "}";
  return out;
}

inline std::string NetworkStreamToJson(const WHANetworkStream& n) {
  std::string out = "{";
  out += "\"ip\":" + EscapeJsonString(n.ip) + ",";
  out += "\"port\":" + std::to_string(n.port) + ",";
  out += "\"codec\":" + std::to_string(static_cast<uint32_t>(n.codec)) + ",";
  char qbuf[32];
  std::snprintf(qbuf, sizeof(qbuf), "%.6g", n.quality);
  out += "\"quality\":" + std::string(qbuf) + ",";
  out += "\"channels\":" + std::to_string(n.channels);
  out += "}";
  return out;
}

inline void SkipWs(std::string_view s, size_t& p) {
  while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
}

inline bool Expect(std::string_view s, size_t& p, char c) {
  SkipWs(s, p);
  if (p >= s.size() || s[p] != c) return false;
  ++p;
  return true;
}

inline bool ParseString(std::string_view s, size_t& p, std::string& out) {
  SkipWs(s, p);
  if (p >= s.size() || s[p] != '"') return false;
  ++p;
  out.clear();
  while (p < s.size()) {
    char c = s[p++];
    if (c == '"') return true;
    if (c == '\\') {
      if (p >= s.size()) return false;
      char e = s[p++];
      if (e == '"' || e == '\\' || e == '/') out.push_back(e);
      else if (e == 'n') out.push_back('\n');
      else if (e == 'r') out.push_back('\r');
      else if (e == 't') out.push_back('\t');
      else if (e == 'u') {
        if (p + 4 > s.size()) return false;
        std::string hex(s.substr(p, 4));
        p += 4;
        int v = 0;
        for (char h : hex) {
          v <<= 4;
          if (h >= '0' && h <= '9') v |= h - '0';
          else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
          else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
          else return false;
        }
        out.push_back(static_cast<char>(v));
      } else return false;
    } else {
      out.push_back(c);
    }
  }
  return false;
}

inline bool ParseInt(std::string_view s, size_t& p, int32_t& out) {
  SkipWs(s, p);
  size_t start = p;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
  while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
  if (p == start) return false;
  if (p < s.size() && s[p] == '-' && start == p - 1) return false;
  std::string tok(s.substr(start, p - start));
  try {
    out = std::stoi(tok);
  } catch (...) {
    return false;
  }
  return true;
}

inline bool ParseUInt(std::string_view s, size_t& p, uint32_t& out) {
  SkipWs(s, p);
  size_t start = p;
  while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
  if (p == start) return false;
  std::string tok(s.substr(start, p - start));
  try {
    out = static_cast<uint32_t>(std::stoul(tok));
  } catch (...) {
    return false;
  }
  return true;
}

inline bool ParseFloat(std::string_view s, size_t& p, float& out) {
  SkipWs(s, p);
  size_t start = p;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
  bool hasDigit = false;
  while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
    ++p;
    hasDigit = true;
  }
  if (p < s.size() && s[p] == '.') {
    ++p;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
      ++p;
      hasDigit = true;
    }
  }
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    ++p;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    bool expDigit = false;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
      ++p;
      expDigit = true;
    }
    if (!expDigit) return false;
  }
  if (!hasDigit) return false;
  std::string tok(s.substr(start, p - start));
  try {
    out = std::stof(tok);
  } catch (...) {
    return false;
  }
  return true;
}

inline bool ParseBool(std::string_view s, size_t& p, bool& out) {
  SkipWs(s, p);
  if (s.size() >= p + 4 && s.substr(p, 4) == "true") {
    p += 4;
    out = true;
    return true;
  }
  if (s.size() >= p + 5 && s.substr(p, 5) == "false") {
    p += 5;
    out = false;
    return true;
  }
  return false;
}

inline bool ParseSlot(std::string_view s, size_t& p, WHASlot& out, std::string* error) {
  if (!Expect(s, p, '{')) {
    if (error) *error = "slot expected {";
    return false;
  }
  out = WHASlot{};
  bool haveType = false, haveSrc = false, haveStream = false, haveName = false, haveEnabled = false,
       haveLoopback = false;
  std::string rawName;
  bool rawNameTooLong = false;
  while (true) {
    SkipWs(s, p);
    if (p < s.size() && s[p] == '}') {
      ++p;
      break;
    }
    std::string key;
    if (!ParseString(s, p, key)) {
      if (error) *error = "slot key parse";
      return false;
    }
    if (!Expect(s, p, ':')) {
      if (error) *error = "slot expected :";
      return false;
    }
    if (key == "type") {
      uint32_t v;
      if (!ParseUInt(s, p, v)) {
        if (error) *error = "slot type parse";
        return false;
      }
      out.type = static_cast<WHASlotType>(v);
      haveType = true;
    } else if (key == "srcChannel") {
      int32_t v;
      if (!ParseInt(s, p, v)) {
        if (error) *error = "slot srcChannel parse";
        return false;
      }
      out.srcChannel = v;
      haveSrc = true;
    } else if (key == "streamId") {
      int32_t v;
      if (!ParseInt(s, p, v)) {
        if (error) *error = "slot streamId parse";
        return false;
      }
      out.streamId = v;
      haveStream = true;
    } else if (key == "name") {
      std::string v;
      if (!ParseString(s, p, v)) {
        if (error) *error = "slot name parse";
        return false;
      }
      if (v.size() >= WHA_NAME_LEN) rawNameTooLong = true;
      rawName = v;
      // Truncate for storage but remember overlong for validation
      SetSlotName(out, v.c_str());
      haveName = true;
    } else if (key == "enabled") {
      bool v;
      if (!ParseBool(s, p, v)) {
        if (error) *error = "slot enabled parse";
        return false;
      }
      out.enabled = v;
      haveEnabled = true;
    } else if (key == "loopback") {
      bool v;
      if (!ParseBool(s, p, v)) {
        if (error) *error = "slot loopback parse";
        return false;
      }
      out.loopback = v;
      haveLoopback = true;
    } else {
      if (error) *error = "slot unknown key";
      return false;
    }
    SkipWs(s, p);
    if (p < s.size() && s[p] == ',') {
      ++p;
      continue;
    }
    if (p < s.size() && s[p] == '}') continue;
    if (error) *error = "slot object";
    return false;
  }
  if (!haveType || !haveSrc || !haveStream || !haveName || !haveEnabled || !haveLoopback) {
    if (error) *error = "slot missing fields";
    return false;
  }
  if (rawNameTooLong) {
    if (error) *error = "slot name too long";
    return false;
  }
  return true;
}

inline bool ParseNetworkStream(std::string_view s, size_t& p, WHANetworkStream& out,
                               std::string* error) {
  if (!Expect(s, p, '{')) {
    if (error) *error = "net expected {";
    return false;
  }
  out = WHANetworkStream{};
  bool haveIp = false, havePort = false, haveCodec = false, haveQuality = false,
       haveChannels = false;
  std::string rawIp;
  bool rawIpTooLong = false;
  while (true) {
    SkipWs(s, p);
    if (p < s.size() && s[p] == '}') {
      ++p;
      break;
    }
    std::string key;
    if (!ParseString(s, p, key)) {
      if (error) *error = "net key parse";
      return false;
    }
    if (!Expect(s, p, ':')) {
      if (error) *error = "net expected :";
      return false;
    }
    if (key == "ip") {
      std::string v;
      if (!ParseString(s, p, v)) {
        if (error) *error = "net ip parse";
        return false;
      }
      if (v.size() >= sizeof(out.ip)) rawIpTooLong = true;
      rawIp = v;
      // Copy safely
      size_t i = 0;
      for (; i + 1 < sizeof(out.ip) && i < v.size(); ++i) out.ip[i] = v[i];
      out.ip[i] = '\0';
      for (++i; i < sizeof(out.ip); ++i) out.ip[i] = '\0';
      haveIp = true;
    } else if (key == "port") {
      uint32_t v;
      if (!ParseUInt(s, p, v)) {
        if (error) *error = "net port parse";
        return false;
      }
      out.port = static_cast<uint16_t>(v);
      havePort = true;
    } else if (key == "codec") {
      uint32_t v;
      if (!ParseUInt(s, p, v)) {
        if (error) *error = "net codec parse";
        return false;
      }
      out.codec = static_cast<WHACodec>(v);
      haveCodec = true;
    } else if (key == "quality") {
      float v;
      if (!ParseFloat(s, p, v)) {
        if (error) *error = "net quality parse";
        return false;
      }
      out.quality = v;
      haveQuality = true;
    } else if (key == "channels") {
      uint32_t v;
      if (!ParseUInt(s, p, v)) {
        if (error) *error = "net channels parse";
        return false;
      }
      out.channels = v;
      haveChannels = true;
    } else {
      if (error) *error = "net unknown key";
      return false;
    }
    SkipWs(s, p);
    if (p < s.size() && s[p] == ',') {
      ++p;
      continue;
    }
    if (p < s.size() && s[p] == '}') continue;
    if (error) *error = "net object";
    return false;
  }
  if (!haveIp || !havePort || !haveCodec || !haveQuality || !haveChannels) {
    if (error) *error = "net missing fields";
    return false;
  }
  if (rawIpTooLong) {
    if (error) *error = "net ip too long";
    return false;
  }
  return true;
}

}  // namespace detail

inline std::string SerializeSlots(const WHASlotTable& t) {
  std::string out = "{";
  out += "\"version\":" + std::to_string(t.version) + ",";
  out += "\"masterInCount\":" + std::to_string(t.masterInCount) + ",";
  out += "\"masterIn\":[";
  for (uint32_t i = 0; i < t.masterInCount; ++i) {
    if (i) out += ",";
    out += detail::SlotToJson(t.masterIn[i]);
  }
  out += "],";
  out += "\"masterOutCount\":" + std::to_string(t.masterOutCount) + ",";
  out += "\"masterOut\":[";
  for (uint32_t i = 0; i < t.masterOutCount; ++i) {
    if (i) out += ",";
    out += detail::SlotToJson(t.masterOut[i]);
  }
  out += "],";
  out += "\"general\":{";
  out += "\"sampleRate\":" + std::to_string(t.general.sampleRate) + ",";
  out += "\"bitDepth\":" + std::to_string(t.general.bitDepth) + ",";
  out += "\"asioBuffer\":" + std::to_string(t.general.asioBuffer) + ",";
  out += "\"hwBuffer\":" + std::to_string(t.general.hwBuffer) + ",";
  out += "\"virtualBuffer\":" + std::to_string(t.general.virtualBuffer) + ",";
  out += "\"bridgeBuffer\":[" + std::to_string(t.general.bridgeBuffer[0]) + "," +
         std::to_string(t.general.bridgeBuffer[1]) + "," +
         std::to_string(t.general.bridgeBuffer[2]) + "," +
         std::to_string(t.general.bridgeBuffer[3]) + "],";
  out += "\"networkPcmBuffer\":" + std::to_string(t.general.networkPcmBuffer) + ",";
  out += "\"networkVorbisBuffer\":" + std::to_string(t.general.networkVorbisBuffer) + ",";
  out += "\"jitterPcm\":" + std::to_string(t.general.jitterPcm) + ",";
  out += "\"jitterVorbis\":" + std::to_string(t.general.jitterVorbis) + ",";
  out += "\"virtualCables\":" + std::to_string(t.general.virtualCables) + ",";
  out += "\"virtualName\":" + detail::EscapeJsonString(t.general.virtualName) + ",";
  out += "\"hwRenderId\":" + detail::EscapeJsonString(t.general.hwRenderId) + ",";
  out += "\"hwCaptureId\":" + detail::EscapeJsonString(t.general.hwCaptureId) + ",";
  // HW devices 2 and up of each direction ("" = none), up to the last listed one.
  for (int dir = 0; dir < 2; ++dir) {
    out += dir == 0 ? "\"hwRenderMore\":[" : ",\"hwCaptureMore\":[";
    int count = 1;
    for (int d = 1; d < kHwDevices; ++d)
      if (HwDeviceListed(t, dir == 1, d)) count = d + 1;
    for (int d = 1; d < count; ++d) {
      if (d > 1) out += ",";
      out += detail::EscapeJsonString(HwDeviceId(t, dir == 1, d));
    }
    out += "]";
  }
  // Every listed HW device's mode (WHAHwMode), device 1 first; absent in older files = Exclusive.
  for (int dir = 0; dir < 2; ++dir) {
    out += dir == 0 ? ",\"hwRenderModes\":[" : ",\"hwCaptureModes\":[";
    int count = 1;
    for (int d = 1; d < kHwDevices; ++d)
      if (HwDeviceListed(t, dir == 1, d)) count = d + 1;
    for (int d = 0; d < count; ++d) {
      if (d) out += ",";
      out += std::to_string(static_cast<int>(dir == 0 ? t.hwModes.render[d] : t.hwModes.capture[d]));
    }
    out += "]";
  }
  // Virtual Cable formats. Its presence also marks VIRTUAL srcChannel as cable * 8 + channel.
  out += ",\"cables\":[";
  for (int c = 0; c < kVirtualSlotCables; ++c) {
    if (c) out += ",";
    const WHACableSetting& cs = CableSetting(t, c);
    out += "{\"channels\":" + std::to_string(cs.channels) + ",\"format\":" + std::to_string(cs.format) + "}";
  }
  out += "]";
  out += "},";
  out += "\"netTx\":[";
  for (int i = 0; i < WHA_NET_STREAMS; ++i) {
    if (i) out += ",";
    out += detail::NetworkStreamToJson(t.netTx[i]);
  }
  out += "],";
  out += "\"netRx\":[";
  for (int i = 0; i < WHA_NET_STREAMS; ++i) {
    if (i) out += ",";
    out += detail::NetworkStreamToJson(t.netRx[i]);
  }
  out += "]";
  out += "}";
  return out;
}

inline bool DeserializeSlots(std::string_view s, WHASlotTable& out, std::string* error) {
  auto fail = [&](const char* msg) -> bool {
    if (error) *error = msg;
    return false;
  };
  size_t p = 0;
  detail::SkipWs(s, p);
  if (!detail::Expect(s, p, '{')) return fail("expected {");
  out = WHASlotTable{};
  bool haveVersion = false, haveInCount = false, haveIn = false, haveOutCount = false,
       haveOut = false, haveGeneral = false, haveTx = false, haveRx = false;
  uint32_t tmpInCount = 0, tmpOutCount = 0;
  WHASlot tmpIn[WHA_MAX] = {};
  WHASlot tmpOut[WHA_MAX] = {};
  uint32_t parsedIn = 0, parsedOut = 0;
  bool inCountSeen = false, outCountSeen = false;
  bool haveCables = false;  // absent: a file from before 8-channel cables (VIRTUAL srcChannel = cable * 2 + side)

  while (true) {
    detail::SkipWs(s, p);
    if (p < s.size() && s[p] == '}') {
      ++p;
      break;
    }
    std::string key;
    if (!detail::ParseString(s, p, key)) return fail("expected key string");
    if (!detail::Expect(s, p, ':')) return fail("expected :");
    if (key == "version") {
      uint32_t v;
      if (!detail::ParseUInt(s, p, v)) return fail("version parse");
      out.version = v;
      haveVersion = true;
    } else if (key == "masterInCount") {
      uint32_t v;
      if (!detail::ParseUInt(s, p, v)) return fail("masterInCount parse");
      tmpInCount = v;
      inCountSeen = true;
      haveInCount = true;
    } else if (key == "masterIn") {
      if (!detail::Expect(s, p, '[')) return fail("masterIn expected [");
      detail::SkipWs(s, p);
      parsedIn = 0;
      if (p < s.size() && s[p] == ']') {
        ++p;
      } else {
        while (true) {
          if (parsedIn >= WHA_MAX) return fail("masterIn too many");
          if (!detail::ParseSlot(s, p, tmpIn[parsedIn], error)) return false;
          ++parsedIn;
          detail::SkipWs(s, p);
          if (p < s.size() && s[p] == ',') {
            ++p;
            continue;
          }
          if (p < s.size() && s[p] == ']') {
            ++p;
            break;
          }
          return fail("masterIn array");
        }
      }
      haveIn = true;
    } else if (key == "masterOutCount") {
      uint32_t v;
      if (!detail::ParseUInt(s, p, v)) return fail("masterOutCount parse");
      tmpOutCount = v;
      outCountSeen = true;
      haveOutCount = true;
    } else if (key == "masterOut") {
      if (!detail::Expect(s, p, '[')) return fail("masterOut expected [");
      detail::SkipWs(s, p);
      parsedOut = 0;
      if (p < s.size() && s[p] == ']') {
        ++p;
      } else {
        while (true) {
          if (parsedOut >= WHA_MAX) return fail("masterOut too many");
          if (!detail::ParseSlot(s, p, tmpOut[parsedOut], error)) return false;
          ++parsedOut;
          detail::SkipWs(s, p);
          if (p < s.size() && s[p] == ',') {
            ++p;
            continue;
          }
          if (p < s.size() && s[p] == ']') {
            ++p;
            break;
          }
          return fail("masterOut array");
        }
      }
      haveOut = true;
    } else if (key == "general") {
      if (!detail::Expect(s, p, '{')) return fail("general expected {");
      bool haveSR = false, haveBD = false, haveAB = false, haveHW = false, haveVB = false,
           haveBB = false, havePCM = false, haveVorbis = false, haveJP = false, haveJV = false,
           haveVC = false, haveVN = false;
      while (true) {
        detail::SkipWs(s, p);
        if (p < s.size() && s[p] == '}') {
          ++p;
          break;
        }
        std::string gkey;
        if (!detail::ParseString(s, p, gkey)) return fail("general key");
        if (!detail::Expect(s, p, ':')) return fail("general :");
        if (gkey == "sampleRate") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("sampleRate");
          out.general.sampleRate = v;
          haveSR = true;
        } else if (gkey == "bitDepth") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("bitDepth");
          out.general.bitDepth = v;
          haveBD = true;
        } else if (gkey == "asioBuffer") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("asioBuffer");
          out.general.asioBuffer = v;
          haveAB = true;
        } else if (gkey == "hwBuffer") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("hwBuffer");
          out.general.hwBuffer = v;
          haveHW = true;
        } else if (gkey == "virtualBuffer") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("virtualBuffer");
          out.general.virtualBuffer = v;
          haveVB = true;
        } else if (gkey == "bridgeBuffer") {
          if (!detail::Expect(s, p, '[')) return fail("bridgeBuffer [");
          for (int i = 0; i < 4; ++i) {
            uint32_t v;
            if (!detail::ParseUInt(s, p, v)) return fail("bridgeBuffer val");
            out.general.bridgeBuffer[i] = v;
            detail::SkipWs(s, p);
            if (i < 3) {
              if (!detail::Expect(s, p, ',')) return fail("bridgeBuffer ,");
            }
          }
          if (!detail::Expect(s, p, ']')) return fail("bridgeBuffer ]");
          haveBB = true;
        } else if (gkey == "networkPcmBuffer") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("networkPcmBuffer");
          out.general.networkPcmBuffer = v;
          havePCM = true;
        } else if (gkey == "networkVorbisBuffer") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("networkVorbisBuffer");
          out.general.networkVorbisBuffer = v;
          haveVorbis = true;
        } else if (gkey == "jitterPcm") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("jitterPcm");
          out.general.jitterPcm = v;
          haveJP = true;
        } else if (gkey == "jitterVorbis") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("jitterVorbis");
          out.general.jitterVorbis = v;
          haveJV = true;
        } else if (gkey == "virtualCables") {
          uint32_t v;
          if (!detail::ParseUInt(s, p, v)) return fail("virtualCables");
          out.general.virtualCables = v;
          haveVC = true;
        } else if (gkey == "virtualName") {
          std::string v;
          if (!detail::ParseString(s, p, v)) return fail("virtualName");
          if (v.size() >= 32) return fail("virtualName too long");
          TruncateCopy(out.general.virtualName, 32, v.c_str());
          haveVN = true;
        } else if (gkey == "hwRenderId" || gkey == "hwCaptureId") {  // optional: absent in older files = default device
          std::string v;
          if (!detail::ParseString(s, p, v)) return fail(gkey.c_str());
          if (v.size() >= kEndpointIdLen) return fail("endpoint id too long");
          TruncateCopy(gkey == "hwRenderId" ? out.general.hwRenderId : out.general.hwCaptureId, kEndpointIdLen, v.c_str());
        } else if (gkey == "hwRenderMore" || gkey == "hwCaptureMore") {  // optional: absent = one device each way
          // Devices 2, 3, ... in order; any length up to kHwDevices - 1 (older files hold 3).
          if (!detail::Expect(s, p, '[')) return fail("HW device list [");
          detail::SkipWs(s, p);
          for (int d = 1; p < s.size() && s[p] != ']'; ++d) {
            if (d > 1 && !detail::Expect(s, p, ',')) return fail("HW device list ,");
            if (d >= kHwDevices) return fail("HW device list too long");
            std::string v;
            if (!detail::ParseString(s, p, v)) return fail("HW device list id");
            if (v.size() >= kEndpointIdLen) return fail("endpoint id too long");
            TruncateCopy(HwDeviceId(out, gkey == "hwCaptureMore", d), kEndpointIdLen, v.c_str());
            detail::SkipWs(s, p);
          }
          if (!detail::Expect(s, p, ']')) return fail("HW device list ]");
        } else if (gkey == "hwRenderModes" || gkey == "hwCaptureModes") {  // optional: absent = every device Exclusive
          uint8_t* place = gkey == "hwRenderModes" ? out.hwModes.render : out.hwModes.capture;
          if (!detail::Expect(s, p, '[')) return fail("HW mode list [");
          detail::SkipWs(s, p);
          for (int d = 0; p < s.size() && s[p] != ']'; ++d) {
            if (d > 0 && !detail::Expect(s, p, ',')) return fail("HW mode list ,");
            if (d >= kHwDevices) return fail("HW mode list too long");
            uint32_t v = 0;
            if (!detail::ParseUInt(s, p, v) || v >= kHwModeCount) return fail("HW mode must be 0..2");
            place[d] = static_cast<uint8_t>(v);
            detail::SkipWs(s, p);
          }
          if (!detail::Expect(s, p, ']')) return fail("HW mode list ]");
        } else if (gkey == "cables") {  // optional: absent = every cable stereo float; 8 (older files) or 16
          if (!detail::Expect(s, p, '[')) return fail("cables [");
          for (int c = 0; c < kVirtualSlotCables; ++c) {
            if (c == kVirtualCablesFirst) {  // an older file ends here
              detail::SkipWs(s, p);
              if (p < s.size() && s[p] == ']') break;
            }
            if (c > 0 && !detail::Expect(s, p, ',')) return fail("cables list ,");
            if (!detail::Expect(s, p, '{')) return fail("cables {");
            for (int field = 0; field < 2; ++field) {
              std::string ckey;
              uint32_t v = 0;
              if (!detail::ParseString(s, p, ckey) || !detail::Expect(s, p, ':') || !detail::ParseUInt(s, p, v) || v > 255)
                return fail("cables field");
              if (ckey == "channels") CableSetting(out, c).channels = static_cast<uint8_t>(v);
              else if (ckey == "format") CableSetting(out, c).format = static_cast<uint8_t>(v);
              else return fail("cables unknown field");
              if (field == 0 && !detail::Expect(s, p, ',')) return fail("cables ,");
            }
            if (!detail::Expect(s, p, '}')) return fail("cables }");
          }
          if (!detail::Expect(s, p, ']')) return fail("cables ]");
          haveCables = true;
        } else return fail("unknown general key");
        detail::SkipWs(s, p);
        if (p < s.size() && s[p] == ',') {
          ++p;
          continue;
        }
        if (p < s.size() && s[p] == '}') continue;
        return fail("general object");
      }
      haveGeneral = haveSR && haveBD && haveAB && haveHW && haveVB && haveBB && havePCM &&
                    haveVorbis && haveJP && haveJV && haveVC && haveVN;
      if (!haveGeneral) return fail("general missing fields");
    } else if (key == "netTx") {
      if (!detail::Expect(s, p, '[')) return fail("netTx [");
      for (int i = 0; i < WHA_NET_STREAMS; ++i) {
        if (!detail::ParseNetworkStream(s, p, out.netTx[i], error)) return false;
        detail::SkipWs(s, p);
        if (i < WHA_NET_STREAMS - 1) {
          if (!detail::Expect(s, p, ',')) return fail("netTx ,");
        }
      }
      if (!detail::Expect(s, p, ']')) return fail("netTx ]");
      haveTx = true;
    } else if (key == "netRx") {
      if (!detail::Expect(s, p, '[')) return fail("netRx [");
      for (int i = 0; i < WHA_NET_STREAMS; ++i) {
        if (!detail::ParseNetworkStream(s, p, out.netRx[i], error)) return false;
        detail::SkipWs(s, p);
        if (i < WHA_NET_STREAMS - 1) {
          if (!detail::Expect(s, p, ',')) return fail("netRx ,");
        }
      }
      if (!detail::Expect(s, p, ']')) return fail("netRx ]");
      haveRx = true;
    } else return fail("unknown top key");
    detail::SkipWs(s, p);
    if (p < s.size() && s[p] == ',') {
      ++p;
      continue;
    }
    if (p < s.size() && s[p] == '}') continue;
    return fail("top object");
  }
  detail::SkipWs(s, p);
  if (p != s.size()) return fail("trailing");
  if (!haveVersion || !haveInCount || !haveIn || !haveOutCount || !haveOut || !haveGeneral ||
      !haveTx || !haveRx)
    return fail("missing top fields");
  // Validate counts match arrays
  if (inCountSeen && parsedIn != tmpInCount) return fail("masterInCount mismatch");
  if (outCountSeen && parsedOut != tmpOutCount) return fail("masterOutCount mismatch");
  out.masterInCount = parsedIn;
  out.masterOutCount = parsedOut;
  for (uint32_t i = 0; i < parsedIn; ++i) out.masterIn[i] = tmpIn[i];
  for (uint32_t i = 0; i < parsedOut; ++i) out.masterOut[i] = tmpOut[i];
  if (!haveCables) {  // stereo cables: Virtual N L/R -> channel 1/2 of cable N
    auto convert = [](WHASlot& slot) {
      if (slot.type == SLOT_VIRTUAL && slot.srcChannel >= 0)
        slot.srcChannel = (slot.srcChannel / 2) * kVirtualCableChannels + slot.srcChannel % 2;
    };
    for (uint32_t i = 0; i < parsedIn; ++i) convert(out.masterIn[i]);
    for (uint32_t i = 0; i < parsedOut; ++i) convert(out.masterOut[i]);
  }
  if (!ValidateSlots(out, error)) return false;
  return true;
}

}  // namespace wha
