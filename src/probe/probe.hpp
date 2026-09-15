#pragma once

#include <windows.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include "json.hpp"

namespace wha {
using Microsoft::WRL::ComPtr;
inline std::wstring utf16(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size))
        throw std::runtime_error("UTF-16 conversion failed");
    return result;
}
inline bool same_windows_id(const std::string& left, const std::string& right) {
    const auto a = utf16(left), b = utf16(right);
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
inline std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid UTF-16 from Windows");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr))
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}
inline std::string guid(const GUID& value) {
    wchar_t buffer[40]{};
    StringFromGUID2(value, buffer, 40);
    return utf8(buffer);
}
inline Json error(const char* operation, DWORD code, const char* domain = "win32") {
    auto result = Json::object();
    result["operation"] = operation; result["code"] = code; result["domain"] = domain;
    wchar_t buffer[1024]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer, 1024, nullptr);
    result["message"] = utf8(buffer);
    return result;
}
inline bool checked(HRESULT hr, const char* operation, Json& errors) {
    if (SUCCEEDED(hr)) return true;
    errors.push(error(operation, static_cast<DWORD>(hr), "hresult"));
    return false;
}
struct CloseHandleDeleter {
    void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
struct CoTaskDeleter { void operator()(void* ptr) const { CoTaskMemFree(ptr); } };
template<class T> using CoMemory = std::unique_ptr<T, CoTaskDeleter>;

inline Json wave_format(const WAVEFORMATEX& wave) {
    auto result = Json::object();
    result["format_tag"] = wave.wFormatTag;
    result["channels"] = wave.nChannels;
    result["sample_rate"] = wave.nSamplesPerSec;
    result["container_bits"] = wave.wBitsPerSample;
    result["block_align_bytes"] = wave.nBlockAlign;
    result["average_bytes_per_second"] = wave.nAvgBytesPerSec;
    if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= 22) {
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        result["valid_bits"] = ext.Samples.wValidBitsPerSample;
        result["channel_mask"] = ext.dwChannelMask;
        result["subformat"] = guid(ext.SubFormat);
    }
    return result;
}

Json enumerate_mmdevices();
Json enumerate_ks();
Json enumerate_asio();
Json correlate(const Json& endpoints, const Json& filters);
struct OpenRequest {
    std::wstring filter;
    ULONG pin{}, rate{}, channels{}, bits{}, valid_bits{}, channel_mask{};
    bool looped{}, extensible{true};
};
Json open_pin(const OpenRequest& request, bool& succeeded);
}
