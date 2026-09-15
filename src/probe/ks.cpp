#include "probe.hpp"
#include <winioctl.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <setupapi.h>
#include <devpkey.h>
#include <array>
#include <cstring>
#include <optional>
#include <set>

namespace wha {
namespace {
constexpr DWORD max_property_bytes = 1024 * 1024;
constexpr ULONG max_pins = 1024;
struct DeviceInfoClose { void operator()(void* value) const { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); } };
using DeviceInfo = std::unique_ptr<void, DeviceInfoClose>;

Handle filter_handle(const std::wstring& path, Json& errors) {
    Handle handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        errors.push(error("CreateFile KS filter", GetLastError())); return {};
    }
    return handle;
}

std::vector<std::byte> property(HANDLE filter, ULONG pin, KSPROPERTY_PIN id, Json& errors) {
    KSP_PIN request{};
    request.Property.Set = KSPROPSETID_Pin;
    request.Property.Id = id; request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;
    std::vector<std::byte> data(65536);
    DWORD returned = 0;
    const auto query = [&] {
        return DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request),
            data.data(), static_cast<DWORD>(data.size()), &returned, nullptr);
    };
    if (!query()) {
        DWORD code = GetLastError();
        if (code == ERROR_MORE_DATA || code == ERROR_INSUFFICIENT_BUFFER) {
            // Some drivers only report the required size for a zero-length query.
            DWORD required = 0;
            DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request), nullptr, 0, &required, nullptr);
            if (required > data.size() && required <= max_property_bytes) {
                data.resize(required);
                if (query()) code = ERROR_SUCCESS;
                else code = GetLastError();
            }
        }
        if (code != ERROR_SUCCESS) {
            auto detail = error("IOCTL_KS_PROPERTY pin", code);
            detail["property_id"] = static_cast<ULONG>(id); detail["pin_id"] = pin;
            errors.push(std::move(detail)); return {};
        }
    }
    if (returned > data.size()) {
        errors.push(error("KS property returned size exceeds buffer", ERROR_INVALID_DATA)); return {};
    }
    data.resize(returned); return data;
}

template<class T> std::optional<T> scalar(HANDLE filter, ULONG pin, KSPROPERTY_PIN id, Json& errors) {
    const auto data = property(filter, pin, id, errors);
    if (data.size() < sizeof(T)) {
        auto detail = error("missing or truncated KS scalar", ERROR_INVALID_DATA);
        detail["property_id"] = static_cast<ULONG>(id); detail["pin_id"] = pin;
        errors.push(std::move(detail));
        return {};
    }
    T value{}; std::memcpy(&value, data.data(), sizeof(T)); return value;
}

template<class T> std::optional<T> read(const std::vector<std::byte>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) return {};
    T value{}; std::memcpy(&value, data.data() + offset, sizeof(T)); return value;
}

Json ranges(HANDLE filter, ULONG pin, Json& errors) {
    auto result = Json::array();
    const auto data = property(filter, pin, KSPROPERTY_PIN_DATARANGES, errors);
    if (data.empty()) return result;
    const auto header = read<KSMULTIPLE_ITEM>(data, 0);
    if (!header || header->Size < sizeof(KSMULTIPLE_ITEM) || header->Size > data.size() || header->Count > 4096) {
        errors.push(error("invalid KS range list", ERROR_INVALID_DATA)); return result;
    }
    size_t offset = sizeof(KSMULTIPLE_ITEM);
    for (ULONG index = 0; index < header->Count; ++index) {
        const auto range = read<KSDATARANGE>(data, offset);
        if (!range || offset > header->Size || range->FormatSize < sizeof(KSDATARANGE) ||
            range->FormatSize > header->Size - offset) {
            errors.push(error("invalid KS range size", ERROR_INVALID_DATA)); break;
        }
        auto item = Json::object();
        item["major_format"] = guid(range->MajorFormat); item["subformat"] = guid(range->SubFormat);
        item["specifier"] = guid(range->Specifier); item["format_size"] = range->FormatSize;
        item["evidence"] = "advertised range; combinations and pin opening unverified";
        if (range->MajorFormat == KSDATAFORMAT_TYPE_AUDIO && range->Specifier == KSDATAFORMAT_SPECIFIER_WAVEFORMATEX &&
            range->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
            const auto audio = read<KSDATARANGE_AUDIO>(data, offset);
            item["maximum_channels"] = audio->MaximumChannels;
            item["minimum_bits"] = audio->MinimumBitsPerSample;
            item["maximum_bits"] = audio->MaximumBitsPerSample;
            item["minimum_rate"] = audio->MinimumSampleFrequency;
            item["maximum_rate"] = audio->MaximumSampleFrequency;
        }
        result.push(std::move(item));
        offset += (static_cast<size_t>(range->FormatSize) + 7) & ~size_t{7};
        if (range->Flags & KSDATARANGE_ATTRIBUTES) {
            errors.push(error("KS range attributes not decoded; remaining ranges unprobed", ERROR_NOT_SUPPORTED));
            break;
        }
    }
    return result;
}

std::vector<KSIDENTIFIER> identifiers(HANDLE filter, ULONG pin, KSPROPERTY_PIN id, Json& errors) {
    std::vector<KSIDENTIFIER> result;
    const auto data = property(filter, pin, id, errors);
    if (data.empty()) return result;
    const auto header = read<KSMULTIPLE_ITEM>(data, 0);
    if (!header || header->Size < sizeof(KSMULTIPLE_ITEM) || header->Size > data.size() ||
        header->Count > (header->Size - sizeof(KSMULTIPLE_ITEM)) / sizeof(KSIDENTIFIER)) {
        errors.push(error("invalid KS identifier list", ERROR_INVALID_DATA)); return result;
    }
    for (ULONG index = 0; index < header->Count; ++index)
        result.push_back(*read<KSIDENTIFIER>(data, sizeof(KSMULTIPLE_ITEM) + index * sizeof(KSIDENTIFIER)));
    return result;
}

Json identifier_json(const std::vector<KSIDENTIFIER>& list) {
    auto result = Json::array();
    for (const auto& id : list) {
        auto item = Json::object(); item["set"] = guid(id.Set); item["id"] = id.Id; item["flags"] = id.Flags;
        result.push(std::move(item));
    }
    return result;
}

Json pin_info(HANDLE handle, ULONG id) {
    auto item = Json::object(); auto errors = Json::array();
    item["pin_id"] = id; item["pin_open"] = "not_attempted"; item["stream_verified"] = false;
    item["period"] = "unprobed; requires selected transport and buffer negotiation";
    if (const auto flow = scalar<KSPIN_DATAFLOW>(handle, id, KSPROPERTY_PIN_DATAFLOW, errors)) {
        item["ks_dataflow"] = static_cast<int>(*flow);
        item["direction"] = *flow == KSPIN_DATAFLOW_OUT ? "to_daw" :
                            *flow == KSPIN_DATAFLOW_IN ? "from_daw" : "unknown";
    }
    if (const auto comm = scalar<KSPIN_COMMUNICATION>(handle, id, KSPROPERTY_PIN_COMMUNICATION, errors))
        item["communication"] = static_cast<int>(*comm);
    if (const auto instances = scalar<KSPIN_CINSTANCES>(handle, id, KSPROPERTY_PIN_CINSTANCES, errors)) {
        item["instances_possible"] = instances->PossibleCount; item["instances_current"] = instances->CurrentCount;
    }
    item["interfaces"] = identifier_json(identifiers(handle, id, KSPROPERTY_PIN_INTERFACES, errors));
    item["mediums"] = identifier_json(identifiers(handle, id, KSPROPERTY_PIN_MEDIUMS, errors));
    item["data_ranges"] = ranges(handle, id, errors);
    item["property_queries"] = errors.items().empty() ? "succeeded" : "partial_or_failed";
    item["errors"] = std::move(errors); return item;
}

void device_identity(HDEVINFO devices, SP_DEVINFO_DATA& dev, Json& item, Json& errors) {
    item["device_instance_id"] = Json{};
    std::array<wchar_t, 4096> instance{};
    if (SetupDiGetDeviceInstanceIdW(devices, &dev, instance.data(), static_cast<DWORD>(instance.size()), nullptr))
        item["device_instance_id"] = utf8(instance.data());
    else errors.push(error("SetupDiGetDeviceInstanceId", GetLastError()));
    std::array<wchar_t, 4096> name{}; DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(devices, &dev, SPDRP_DEVICEDESC, &type,
            reinterpret_cast<BYTE*>(name.data()), static_cast<DWORD>(sizeof(name) - sizeof(wchar_t)), nullptr))
        item["name"] = utf8(name.data());
    else errors.push(error("SetupDiGetDeviceRegistryProperty description", GetLastError()));
    GUID container{}; DEVPROPTYPE property_type = 0;
    if (SetupDiGetDevicePropertyW(devices, &dev, &DEVPKEY_Device_ContainerId, &property_type,
            reinterpret_cast<BYTE*>(&container), sizeof(container), nullptr, 0) && property_type == DEVPROP_TYPE_GUID)
        item["container_id"] = guid(container);
    else errors.push(error("SetupDiGetDeviceProperty container", GetLastError()));
}
}

Json enumerate_ks() {
    auto result = Json::object(); auto errors = Json::array(); result["filters"] = Json::array();
    DeviceInfo devices(SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (devices.get() == INVALID_HANDLE_VALUE) {
        errors.push(error("SetupDiGetClassDevs audio", GetLastError()));
        result["errors"] = std::move(errors); return result;
    }
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{}; interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices.get(), nullptr, &KSCATEGORY_AUDIO, index, &interface_data)) {
            const DWORD code = GetLastError();
            if (code != ERROR_NO_MORE_ITEMS) errors.push(error("SetupDiEnumDeviceInterfaces", code));
            break;
        }
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices.get(), &interface_data, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes > 65536) {
            errors.push(error("KS interface path size", ERROR_INVALID_DATA)); continue;
        }
        std::vector<std::byte> storage(bytes + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        if (!SetupDiGetDeviceInterfaceDetailW(devices.get(), &interface_data, detail, bytes, nullptr, &dev)) {
            errors.push(error("SetupDiGetDeviceInterfaceDetail", GetLastError())); continue;
        }
        auto item = Json::object(); auto item_errors = Json::array();
        item["path"] = utf8(detail->DevicePath); item["discovered"] = true;
        item["pins"] = Json::array(); item["stream_verified"] = false;
        item["clock_relationship"] = "unverified; physical identity does not prove shared clock";
        device_identity(devices.get(), dev, item, item_errors);
        const auto handle = filter_handle(detail->DevicePath, item_errors);
        item["filter_handle_open"] = static_cast<bool>(handle);
        if (handle) {
            const auto count = scalar<ULONG>(handle.get(), 0, KSPROPERTY_PIN_CTYPES, item_errors);
            if (count && *count <= max_pins) {
                item["pin_count"] = *count;
                for (ULONG pin = 0; pin < *count; ++pin) item["pins"].push(pin_info(handle.get(), pin));
            } else if (count) item_errors.push(error("KS pin count exceeds probe limit", ERROR_INVALID_DATA));
        }
        item["property_queries"] = item_errors.items().empty() ? "succeeded" : "partial_or_failed";
        item["errors"] = std::move(item_errors); result["filters"].push(std::move(item));
    }
    result["errors"] = std::move(errors); return result;
}

Json open_pin(const OpenRequest& request, bool& succeeded) {
    succeeded = false;
    auto result = Json::object(); auto errors = Json::array();
    result["filter"] = utf8(request.filter); result["pin_id"] = request.pin;
    result["pin_open"] = "not_attempted"; result["stream_verified"] = false;
    result["period"] = "not_negotiated"; result["pin_state_requested"] = "STOP (creation only)";
    result["interface"] = request.looped ? "looped" : "streaming";
    const auto handle = filter_handle(request.filter, errors);
    if (handle) {
        const auto count = scalar<ULONG>(handle.get(), 0, KSPROPERTY_PIN_CTYPES, errors);
        if (!count || request.pin >= *count) errors.push(error("pin ID outside discovered factories", ERROR_INVALID_PARAMETER));
        else {
            const auto flow = scalar<KSPIN_DATAFLOW>(handle.get(), request.pin, KSPROPERTY_PIN_DATAFLOW, errors);
            const auto comm = scalar<KSPIN_COMMUNICATION>(handle.get(), request.pin, KSPROPERTY_PIN_COMMUNICATION, errors);
            const auto interfaces = identifiers(handle.get(), request.pin, KSPROPERTY_PIN_INTERFACES, errors);
            const auto mediums = identifiers(handle.get(), request.pin, KSPROPERTY_PIN_MEDIUMS, errors);
            const ULONG interface_id = request.looped ? KSINTERFACE_STANDARD_LOOPED_STREAMING : KSINTERFACE_STANDARD_STREAMING;
            bool interface_found = false, medium_found = false;
            KSIDENTIFIER selected_interface{}, selected_medium{};
            for (const auto& entry : interfaces) if (entry.Set == KSINTERFACESETID_Standard && entry.Id == interface_id) {
                interface_found = true; selected_interface = entry; break;
            }
            for (const auto& entry : mediums) if (entry.Set == KSMEDIUMSETID_Standard && entry.Id == KSMEDIUM_TYPE_ANYINSTANCE) {
                medium_found = true; selected_medium = entry; break;
            }
            if (!interface_found || !medium_found || !flow || !comm ||
                (*flow != KSPIN_DATAFLOW_IN && *flow != KSPIN_DATAFLOW_OUT) ||
                (*comm != KSPIN_COMMUNICATION_SINK && *comm != KSPIN_COMMUNICATION_BOTH)) {
                errors.push(error("selected interface/medium or user-mode pin communication unsupported", ERROR_NOT_SUPPORTED));
            } else {
                // KsCreatePin requires the format immediately after KSPIN_CONNECT.
                struct Connection { KSPIN_CONNECT connect; KSDATAFORMAT_WAVEFORMATEXTENSIBLE format; } connection{};
                static_assert(offsetof(Connection, format) == sizeof(KSPIN_CONNECT));
                connection.connect.Interface = selected_interface; connection.connect.Medium = selected_medium;
                connection.connect.PinId = request.pin; connection.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
                connection.connect.Priority.PrioritySubClass = 1;
                auto& format = connection.format;
                format.DataFormat.FormatSize = request.extensible ? sizeof(format) : sizeof(KSDATAFORMAT_WAVEFORMATEX);
                format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
                format.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
                auto& wave = format.WaveFormatExt.Format;
                wave.wFormatTag = request.extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
                wave.nChannels = static_cast<WORD>(request.channels); wave.nSamplesPerSec = request.rate;
                wave.wBitsPerSample = static_cast<WORD>(request.bits);
                wave.nBlockAlign = static_cast<WORD>(request.channels * (request.bits / 8));
                wave.nAvgBytesPerSec = request.rate * wave.nBlockAlign;
                wave.cbSize = request.extensible ? 22 : 0;
                format.WaveFormatExt.Samples.wValidBitsPerSample = static_cast<WORD>(request.valid_bits);
                format.WaveFormatExt.dwChannelMask = request.channel_mask;
                format.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                format.DataFormat.SampleSize = wave.nBlockAlign;
                result["requested_format"] = wave_format(wave);
                HANDLE raw_pin = nullptr;
                const DWORD code = KsCreatePin(handle.get(), &connection.connect,
                    GENERIC_READ | GENERIC_WRITE, &raw_pin);
                Handle pin(raw_pin);
                succeeded = code == ERROR_SUCCESS && pin && pin.get() != INVALID_HANDLE_VALUE;
                result["pin_open"] = succeeded ? "succeeded" : "failed";
                result["direction"] = *flow == KSPIN_DATAFLOW_IN ? "from_daw" : "to_daw";
                if (code != ERROR_SUCCESS) errors.push(error("KsCreatePin", code));
                else if (!succeeded) errors.push(error("KsCreatePin returned invalid handle", ERROR_INVALID_HANDLE));
                // No state transitions, allocator setup, reads, writes, or audio.
            }
        }
    }
    result["errors"] = std::move(errors); return result;
}
}
