#pragma once

// WHAYaml — the small YAML subset the config files use (routes.yml, settings.yml). Offline, header-only.
// Supported: # comments, block maps ("key: value"), block lists ("- item", "- key: value" maps),
// flow lists of scalars ("[a, b]"), plain, "double" and 'single' quoted scalars, LF or CRLF.
// Not supported (an error names the line): tab indents, flow maps, anchors/aliases/tags, block
// scalars (| >), multi-line strings, several documents.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace wha {

struct YamlNode {
  enum class Kind { Scalar, Map, Seq };
  Kind kind = Kind::Scalar;
  std::string scalar;             // Scalar: the text, quotes removed
  bool quoted = false;            // Scalar: was written in quotes (always a string)
  std::vector<std::string> keys;  // Map: keys, in file order
  std::vector<YamlNode> items;    // Map: values (same order as keys); Seq: items
  int line = 0;                   // 1-based line it starts on

  bool IsMap() const { return kind == Kind::Map; }
  bool IsSeq() const { return kind == Kind::Seq; }
  bool IsScalar() const { return kind == Kind::Scalar; }
  const YamlNode* Find(std::string_view key) const {
    for (size_t i = 0; i < keys.size(); ++i)
      if (keys[i] == key) return &items[i];
    return nullptr;
  }
};

// A string as a double-quoted YAML scalar ("yes", "1:2" and "#x" stay strings).
inline std::string YamlQuote(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20 || c == 0x7F) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
  return out;
}

namespace yaml_detail {

struct Line {
  int number = 0;   // 1-based
  int indent = 0;   // leading spaces
  std::string text; // without indent, comment and trailing spaces
};

inline bool Fail(std::string* error, int line, const std::string& msg) {
  if (error) *error = "line " + std::to_string(line) + ": " + msg;
  return false;
}

inline void AppendUtf8(std::string& out, uint32_t v) {
  if (v < 0x80) {
    out.push_back(static_cast<char>(v));
  } else if (v < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (v >> 6)));
    out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (v >> 12)));
    out.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
  }
}

// Reads a quoted scalar starting at s[p] (a quote); p ends past the closing quote.
inline bool ReadQuoted(std::string_view s, size_t& p, std::string& out, int line, std::string* error) {
  const char q = s[p++];
  out.clear();
  while (p < s.size()) {
    const char c = s[p++];
    if (c == q) {
      if (q == '\'' && p < s.size() && s[p] == '\'') {  // '' inside single quotes
        out.push_back('\'');
        ++p;
        continue;
      }
      return true;
    }
    if (c != '\\' || q == '\'') {
      out.push_back(c);
      continue;
    }
    if (p >= s.size()) break;
    const char e = s[p++];
    int hexDigits = 0;
    switch (e) {
      case '"': case '\\': case '/': out.push_back(e); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case '0': out.push_back('\0'); break;
      case 'x': hexDigits = 2; break;
      case 'u': hexDigits = 4; break;
      default: return Fail(error, line, std::string("unknown escape \\") + e);
    }
    if (hexDigits) {
      if (p + hexDigits > s.size()) return Fail(error, line, "short \\x or \\u escape");
      uint32_t v = 0;
      for (int i = 0; i < hexDigits; ++i) {
        const char h = s[p++];
        v <<= 4;
        if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
        else return Fail(error, line, "bad hex digit in escape");
      }
      AppendUtf8(out, v);
    }
  }
  return Fail(error, line, "missing closing quote");
}

inline std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// One scalar: quoted (nothing may follow) or plain.
inline bool ReadScalar(std::string_view text, YamlNode& out, int line, std::string* error) {
  text = Trim(text);
  out = YamlNode{};
  out.line = line;
  if (text.empty()) return true;  // "key:" with nothing: an empty (null) scalar
  if (text.front() == '"' || text.front() == '\'') {
    size_t p = 0;
    if (!ReadQuoted(text, p, out.scalar, line, error)) return false;
    if (!Trim(text.substr(p)).empty()) return Fail(error, line, "text after a closing quote");
    out.quoted = true;
    return true;
  }
  switch (text.front()) {
    case '{': return Fail(error, line, "flow maps { } are not supported; use one key per line");
    case '&': case '*': case '!': return Fail(error, line, "anchors, aliases and tags are not supported");
    case '|': case '>': return Fail(error, line, "block scalars | > are not supported; use a quoted string");
    default: break;
  }
  out.scalar.assign(text);
  return true;
}

// A value after "key:" or "- ": a flow list [a, b] or a scalar.
inline bool ReadInline(std::string_view text, YamlNode& out, int line, std::string* error) {
  text = Trim(text);
  if (text.empty() || text.front() != '[') return ReadScalar(text, out, line, error);
  out = YamlNode{};
  out.kind = YamlNode::Kind::Seq;
  out.line = line;
  size_t p = 1;
  bool expectItem = true;
  for (;;) {
    while (p < text.size() && text[p] == ' ') ++p;
    if (p >= text.size()) return Fail(error, line, "missing ]");
    if (text[p] == ']') {
      if (expectItem && !out.items.empty()) return Fail(error, line, "empty item before ]");
      ++p;
      break;
    }
    if (!expectItem) return Fail(error, line, "expected , or ]");
    YamlNode item;
    item.line = line;
    if (text[p] == '"' || text[p] == '\'') {
      if (!ReadQuoted(text, p, item.scalar, line, error)) return false;
      item.quoted = true;
    } else if (text[p] == '[' || text[p] == '{') {
      return Fail(error, line, "nested lists are not supported");
    } else {
      const size_t start = p;
      while (p < text.size() && text[p] != ',' && text[p] != ']') ++p;
      item.scalar.assign(Trim(text.substr(start, p - start)));
      if (item.scalar.empty()) return Fail(error, line, "empty list item");
    }
    out.items.push_back(std::move(item));
    while (p < text.size() && text[p] == ' ') ++p;
    if (p < text.size() && text[p] == ',') {
      ++p;
      expectItem = true;
    } else {
      expectItem = false;
    }
  }
  if (!Trim(text.substr(p)).empty()) return Fail(error, line, "text after ]");
  return true;
}

// Splits "key: rest". False when the text is not a map entry (no ": " and no trailing ':').
inline bool SplitKey(std::string_view text, std::string_view& key, std::string_view& rest) {
  if (text.empty() || text.front() == '"' || text.front() == '\'' || text.front() == '[') return false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != ':') continue;
    if (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\t') {
      key = Trim(text.substr(0, i));
      rest = i + 1 < text.size() ? text.substr(i + 1) : std::string_view();
      return !key.empty();
    }
  }
  return false;
}

inline bool IsItem(const Line& l) { return l.text == "-" || l.text.rfind("- ", 0) == 0; }

class Parser {
 public:
  Parser(std::vector<Line> lines, std::string* error) : lines_(std::move(lines)), error_(error) {}

  bool ParseDocument(YamlNode& root) {
    if (lines_.empty()) {
      root = YamlNode{};
      root.kind = YamlNode::Kind::Map;
      return true;
    }
    size_t i = 0;
    if (lines_[0].indent != 0) return Fail(error_, lines_[0].number, "the first line must not be indented");
    if (!ParseBlock(i, 0, root)) return false;
    if (i < lines_.size()) return Fail(error_, lines_[i].number, "unexpected indent");
    return true;
  }

 private:
  bool ParseBlock(size_t& i, int indent, YamlNode& out) {
    return IsItem(lines_[i]) ? ParseSeq(i, indent, out) : ParseMap(i, indent, out);
  }

  // The value of "key:" / "- " with nothing after it: the indented block below, or an empty scalar.
  bool ParseNested(size_t& i, int parentIndent, bool allowSameIndentList, YamlNode& out, int line) {
    if (i < lines_.size() &&
        (lines_[i].indent > parentIndent || (allowSameIndentList && lines_[i].indent == parentIndent && IsItem(lines_[i]))))
      return ParseBlock(i, lines_[i].indent, out);
    out = YamlNode{};
    out.line = line;
    return true;
  }

  bool ParseMap(size_t& i, int indent, YamlNode& out) {
    out = YamlNode{};
    out.kind = YamlNode::Kind::Map;
    out.line = lines_[i].number;
    while (i < lines_.size() && lines_[i].indent == indent) {
      const Line& l = lines_[i];
      if (IsItem(l)) return Fail(error_, l.number, "a list item where a key was expected");
      std::string_view key, rest;
      if (!SplitKey(l.text, key, rest)) return Fail(error_, l.number, "expected \"key: value\"");
      if (out.Find(key)) return Fail(error_, l.number, "duplicate key \"" + std::string(key) + "\"");
      YamlNode value;
      const int number = l.number;
      ++i;
      if (Trim(rest).empty()) {
        if (!ParseNested(i, indent, true, value, number)) return false;
      } else if (!ReadInline(rest, value, number, error_)) {
        return false;
      }
      out.keys.emplace_back(key);
      out.items.push_back(std::move(value));
    }
    if (i < lines_.size() && lines_[i].indent > indent) return Fail(error_, lines_[i].number, "unexpected indent");
    return true;
  }

  bool ParseSeq(size_t& i, int indent, YamlNode& out) {
    out = YamlNode{};
    out.kind = YamlNode::Kind::Seq;
    out.line = lines_[i].number;
    while (i < lines_.size() && lines_[i].indent == indent && IsItem(lines_[i])) {
      Line& l = lines_[i];
      const int number = l.number;
      size_t skip = 1;
      while (skip < l.text.size() && l.text[skip] == ' ') ++skip;
      const std::string rest = l.text.substr(skip);
      YamlNode item;
      std::string_view key, value;
      if (rest.empty()) {
        ++i;
        if (!ParseNested(i, indent, false, item, number)) return false;
      } else if (rest.front() == '-' && (rest.size() == 1 || rest[1] == ' ')) {
        return Fail(error_, number, "nested lists are not supported");
      } else if (SplitKey(rest, key, value)) {
        // "- key: value": a map whose keys line up with this first key.
        l.indent = indent + static_cast<int>(skip);
        l.text = rest;
        if (!ParseMap(i, l.indent, item)) return false;
      } else {
        if (!ReadInline(rest, item, number, error_)) return false;
        ++i;
      }
      out.items.push_back(std::move(item));
    }
    if (i < lines_.size() && lines_[i].indent > indent) return Fail(error_, lines_[i].number, "unexpected indent");
    return true;
  }

  std::vector<Line> lines_;
  std::string* error_;
};

// Splits text into meaningful lines: no blank or comment-only lines, comments cut, CR removed.
inline bool SplitLines(std::string_view text, std::vector<Line>& lines, std::string* error) {
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // UTF-8 BOM
  int number = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view raw = text.substr(start, end - start);
    start = end + 1;
    ++number;
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    // Cut a comment: '#' at the start or after a space, outside quotes.
    char quote = 0;
    size_t cut = raw.size();
    for (size_t k = 0; k < raw.size(); ++k) {
      const char c = raw[k];
      if (quote) {
        if (quote == '"' && c == '\\') ++k;
        else if (quote == '\'' && c == '\'' && k + 1 < raw.size() && raw[k + 1] == '\'') ++k;  // '' = a quote
        else if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        if (k == 0 || raw[k - 1] == ' ' || raw[k - 1] == '[' || raw[k - 1] == ',' || raw[k - 1] == ':' || raw[k - 1] == '-')
          quote = c;
      } else if (c == '#' && (k == 0 || raw[k - 1] == ' ' || raw[k - 1] == '\t')) {
        cut = k;
        break;
      }
    }
    raw = raw.substr(0, cut);
    int indent = 0;
    while (static_cast<size_t>(indent) < raw.size() && raw[indent] == ' ') ++indent;
    std::string_view body = Trim(raw.substr(indent));
    if (body.empty()) {
      if (end == text.size()) break;
      continue;
    }
    if (raw[indent] == '\t') return Fail(error, number, "tabs cannot indent; use spaces");
    if (body == "---" && indent == 0 && lines.empty()) continue;  // document start marker
    if (body == "---" || body == "...") return Fail(error, number, "only one document per file");
    lines.push_back(Line{number, indent, std::string(body)});
    if (end == text.size()) break;
  }
  return true;
}

}  // namespace yaml_detail

// Parses a YAML-subset document; the root of an empty document is an empty map.
inline bool ParseYaml(std::string_view text, YamlNode& root, std::string* error) {
  std::vector<yaml_detail::Line> lines;
  if (!yaml_detail::SplitLines(text, lines, error)) return false;
  yaml_detail::Parser parser(std::move(lines), error);
  return parser.ParseDocument(root);
}

}  // namespace wha
