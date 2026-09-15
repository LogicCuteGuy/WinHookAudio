#include "probe.hpp"
#include <bit>
#include <charconv>
#include <iostream>
#include <set>

namespace {
constexpr const char* usage =
    "winhookaudio-probe discover\n"
    "winhookaudio-probe open --filter <KS path> --pin <id> --rate <Hz>\n"
    "  --channels <1..64> --bits <16|24|32> --valid-bits <1..bits>\n"
    "  --channel-mask <decimal> --interface <streaming|looped>\n"
    "  --wave-format <pcm|extensible>\n"
    "Discovery queries properties only. Open creates and closes one STOP-state PCM\n"
    "pin; it never starts audio. Numeric arguments are unsigned decimal.\n";

ULONG number(const std::wstring& value) {
    const auto text = wha::utf8(value);
    ULONG result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Expected unsigned decimal integer");
    return result;
}

wha::OpenRequest parse_open(int argc, wchar_t** argv) {
    wha::OpenRequest request;
    std::set<std::wstring> seen;
    for (int i = 2; i < argc; i += 2) {
        const std::wstring key = argv[i];
        if (i + 1 == argc || !seen.insert(key).second)
            throw std::invalid_argument("Missing value or duplicate option");
        const std::wstring value = argv[i + 1];
        if (key == L"--filter") request.filter = value;
        else if (key == L"--pin") request.pin = number(value);
        else if (key == L"--rate") request.rate = number(value);
        else if (key == L"--channels") request.channels = number(value);
        else if (key == L"--bits") request.bits = number(value);
        else if (key == L"--valid-bits") request.valid_bits = number(value);
        else if (key == L"--channel-mask") request.channel_mask = number(value);
        else if (key == L"--interface") {
            if (value != L"streaming" && value != L"looped") throw std::invalid_argument("Invalid interface");
            request.looped = value == L"looped";
        } else if (key == L"--wave-format") {
            if (value != L"pcm" && value != L"extensible") throw std::invalid_argument("Invalid wave format");
            request.extensible = value == L"extensible";
        } else throw std::invalid_argument("Unknown option");
    }
    if (seen.size() != 9 || request.filter.empty()) throw std::invalid_argument("All nine open options are required");
    if (request.channels < 1 || request.channels > 64 || request.rate < 8000 || request.rate > 768000 ||
        (request.bits != 16 && request.bits != 24 && request.bits != 32) || request.valid_bits < 1 || request.valid_bits > request.bits)
        throw std::invalid_argument("Format outside probe limits (8k..768k Hz, 1..64 channels, PCM16/24/32)");
    if (request.channel_mask && static_cast<ULONG>(std::popcount(request.channel_mask)) != request.channels)
        throw std::invalid_argument("Channel mask population must equal channel count, or use zero for unspecified positions");
    if (!request.extensible && (request.valid_bits != request.bits || request.channel_mask || request.channels > 2))
        throw std::invalid_argument("Legacy PCM requires <=2 channels, equal valid/container bits, and zero channel mask");
    return request;
}

struct ComSession {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComSession() { if (SUCCEEDED(result)) CoUninitialize(); }
};

wha::Json envelope(const char* operation) {
    auto result = wha::Json::object();
    result["schema_version"] = 1; result["operation"] = operation;
    result["architecture"] = "x64"; result["stream_verified"] = false;
    SYSTEMTIME now{}; GetSystemTime(&now);
    char stamp[40]{};
    sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    result["captured_at_utc"] = stamp;
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        const std::wstring command = argc > 1 ? argv[1] : L"discover";
        if (command == L"--help" && argc == 2) { std::cout << usage; return 0; }
        if (command == L"open") {
            const auto request = parse_open(argc, argv); // validate everything before touching a device
            auto result = envelope("open"); bool succeeded = false;
            result["result"] = wha::open_pin(request, succeeded);
            result.write(std::cout); std::cout << '\n'; return succeeded ? 0 : 3;
        }
        if (command != L"discover" || argc > 2) throw std::invalid_argument("Unknown command or unexpected arguments");
        const ComSession com;
        if (FAILED(com.result)) {
            auto result = envelope("discover");
            result["error"] = wha::error("CoInitializeEx", static_cast<DWORD>(com.result), "hresult");
            result.write(std::cout); std::cout << '\n'; return 1;
        }
        auto result = envelope("discover");
        result["mmdevice"] = wha::enumerate_mmdevices();
        result["ks"] = wha::enumerate_ks();
        result["asio"] = wha::enumerate_asio();
        result["api_view_matches"] = wha::correlate(result["mmdevice"], result["ks"]);
        result["unmatched_identity_policy"] = "unresolved; do not infer distinct hardware or shared clocks";
        result.write(std::cout); std::cout << '\n';
        return 0;
    } catch (const std::invalid_argument& exception) {
        auto result = envelope("invalid_arguments"); result["message"] = exception.what();
        result.write(std::cout); std::cout << '\n'; std::cerr << usage; return 2;
    } catch (const std::exception& exception) {
        auto result = envelope("fatal_error"); result["message"] = exception.what();
        result.write(std::cout); std::cout << '\n'; return 1;
    }
}
