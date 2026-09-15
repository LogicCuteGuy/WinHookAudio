#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace wha {
// Small output-only JSON value. Strings supplied here are UTF-8.
class Json {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    Json() = default;
    Json(bool value) : value_(value) {}
    Json(std::int64_t value) : value_(value) {}
    Json(unsigned long value) : value_(static_cast<std::int64_t>(value)) {}
    Json(unsigned int value) : value_(static_cast<std::int64_t>(value)) {}
    Json(int value) : value_(static_cast<std::int64_t>(value)) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    static Json object() { return Object{}; }
    static Json array() { return Array{}; }
    Json& operator[](const std::string& key) { return std::get<Object>(value_)[key]; }
    void push(Json value) { std::get<Array>(value_).push_back(std::move(value)); }
    const Array& items() const { return std::get<Array>(value_); }
    const Json& at(const std::string& key) const { return std::get<Object>(value_).at(key); }
    const std::string& string() const { return std::get<std::string>(value_); }
    const std::string* string_if() const { return std::get_if<std::string>(&value_); }

    void write(std::ostream& out) const {
        std::visit([&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) out << "null";
            else if constexpr (std::is_same_v<T, bool>) out << (value ? "true" : "false");
            else if constexpr (std::is_same_v<T, std::string>) quote(out, value);
            else if constexpr (std::is_same_v<T, Object>) {
                out << '{'; bool first = true;
                for (const auto& [key, child] : value) {
                    if (!first) out << ',';
                    first = false; quote(out, key); out << ':'; child.write(out);
                }
                out << '}';
            } else if constexpr (std::is_same_v<T, Array>) {
                out << '['; bool first = true;
                for (const auto& child : value) {
                    if (!first) out << ',';
                    first = false; child.write(out);
                }
                out << ']';
            } else out << value;
        }, value_);
    }
private:
    static void quote(std::ostream& out, const std::string& value) {
        constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (unsigned char c : value) {
            if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
            else if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
            else out << static_cast<char>(c);
        }
        out << '"';
    }
    std::variant<std::monostate, bool, std::int64_t, std::string, Object, Array> value_;
};
}
