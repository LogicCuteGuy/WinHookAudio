#include "probe.hpp"
#include <array>

namespace wha {
namespace {
struct RegClose { void operator()(HKEY key) const { if (key) RegCloseKey(key); } };
using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, RegClose>;
Json registry_string(HKEY key, const wchar_t* name, Json& errors) {
    DWORD bytes = 0;
    auto code = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                             nullptr, nullptr, &bytes);
    if (code != ERROR_SUCCESS) {
        errors.push(error(utf8(name).c_str(), code)); return {};
    }
    if (bytes > 65536 || bytes % sizeof(wchar_t)) {
        errors.push(error("registry string size", ERROR_INVALID_DATA)); return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    code = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       nullptr, buffer.data(), &bytes);
    if (code != ERROR_SUCCESS) { errors.push(error(utf8(name).c_str(), code)); return {}; }
    return utf8(buffer.data());
}
}
Json enumerate_asio() {
    auto result = Json::object(); auto errors = Json::array(); result["registrations"] = Json::array();
    for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY raw = nullptr;
        const auto code = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | view, &raw);
        Key root(raw);
        if (code == ERROR_FILE_NOT_FOUND) continue;
        if (code != ERROR_SUCCESS) { errors.push(error("RegOpenKey ASIO", code)); continue; }
        for (DWORD index = 0;; ++index) {
            std::array<wchar_t, 256> name{}; DWORD length = static_cast<DWORD>(name.size());
            const auto next = RegEnumKeyExW(root.get(), index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
            if (next == ERROR_NO_MORE_ITEMS) break;
            if (next != ERROR_SUCCESS) { errors.push(error("RegEnumKey ASIO", next)); break; }
            auto item = Json::object(); auto item_errors = Json::array();
            item["name"] = utf8(name.data()); item["registry_view"] = view == KEY_WOW64_64KEY ? "x64" : "x86";
            item["registered"] = true; item["driver_loaded"] = false; item["stream_verified"] = false;
            HKEY raw_driver = nullptr;
            const auto opened = RegOpenKeyExW(root.get(), name.data(), 0, KEY_READ | view, &raw_driver);
            Key driver(raw_driver);
            if (opened == ERROR_SUCCESS) {
                item["clsid"] = registry_string(driver.get(), L"CLSID", item_errors);
                item["description"] = registry_string(driver.get(), L"Description", item_errors);
            } else item_errors.push(error("RegOpenKey ASIO driver", opened));
            item["errors"] = std::move(item_errors);
            result["registrations"].push(std::move(item));
        }
    }
    result["errors"] = std::move(errors); return result;
}
}
