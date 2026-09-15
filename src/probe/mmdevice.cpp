#include "probe.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace wha {
namespace {
void read_property(IPropertyStore* store, REFPROPERTYKEY key, const char* name,
                   Json& destination, Json& errors) {
    PROPVARIANT value{};
    const HRESULT hr = store->GetValue(key, &value);
    if (checked(hr, name, errors)) {
        if (value.vt == VT_LPWSTR && value.pwszVal) destination[name] = utf8(value.pwszVal);
        else if (value.vt == VT_CLSID && value.puuid) destination[name] = guid(*value.puuid);
        else if (value.vt == VT_EMPTY) destination[name] = Json{};
        else errors.push(error(name, ERROR_DATATYPE_MISMATCH));
    }
    PropVariantClear(&value);
}

Json endpoint(IMMDevice* device, IMMDeviceEnumerator* enumerator) {
    auto item = Json::object(); auto errors = Json::array();
    item["discovered"] = true;
    item["pin_open"] = "not_attempted";
    item["stream_verified"] = false;
    item["clock_relationship"] = "unverified";
    item["connected_adapters"] = Json::array(); item["id"] = Json{};
    LPWSTR raw_id = nullptr;
    const HRESULT id_hr = device->GetId(&raw_id);
    CoMemory<wchar_t> id(raw_id);
    if (checked(id_hr, "IMMDevice::GetId", errors)) item["id"] = utf8(id.get());
    DWORD state = 0;
    if (checked(device->GetState(&state), "IMMDevice::GetState", errors)) item["state"] = state;
    ComPtr<IMMEndpoint> direction;
    if (checked(device->QueryInterface(IID_PPV_ARGS(&direction)), "IMMEndpoint", errors)) {
        EDataFlow flow{};
        if (checked(direction->GetDataFlow(&flow), "GetDataFlow", errors))
            item["direction"] = flow == eCapture ? "to_daw" : "from_daw";
    }
    ComPtr<IPropertyStore> store;
    if (checked(device->OpenPropertyStore(STGM_READ, &store), "OpenPropertyStore", errors)) {
        read_property(store.Get(), PKEY_Device_FriendlyName, "name", item, errors);
        read_property(store.Get(), PKEY_Device_InstanceId, "device_instance_id", item, errors);
        read_property(store.Get(), PKEY_Device_ContainerId, "container_id", item, errors);
    }
    // A connector's connected-device ID identifies the adapter topology. It may
    // match a KS interface exactly; container/name matches are not clock proof.
    ComPtr<IDeviceTopology> topology;
    if (checked(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(topology.GetAddressOf())), "Activate topology", errors)) {
        UINT count = 0;
        if (checked(topology->GetConnectorCount(&count), "GetConnectorCount", errors)) {
            if (count > 256) errors.push(error("connector count exceeds probe limit", ERROR_INVALID_DATA));
            else for (UINT index = 0; index < count; ++index) {
                ComPtr<IConnector> connector;
                if (!checked(topology->GetConnector(index, &connector), "GetConnector", errors)) continue;
                LPWSTR raw = nullptr;
                const HRESULT hr = connector->GetDeviceIdConnectedTo(&raw);
                CoMemory<wchar_t> connected(raw);
                if (checked(hr, "GetDeviceIdConnectedTo", errors) && connected) {
                    auto adapter = Json::object(); adapter["id"] = utf8(connected.get());
                    adapter["device_instance_id"] = Json{};
                    ComPtr<IMMDevice> adapter_device;
                    if (checked(enumerator->GetDevice(connected.get(), &adapter_device), "GetDevice connected adapter", errors)) {
                        ComPtr<IPropertyStore> adapter_store;
                        if (checked(adapter_device->OpenPropertyStore(STGM_READ, &adapter_store), "OpenPropertyStore adapter", errors))
                            read_property(adapter_store.Get(), PKEY_Device_InstanceId, "device_instance_id", adapter, errors);
                    }
                    item["connected_adapters"].push(std::move(adapter));
                }
            }
        }
    }
    item["wasapi"] = Json::object();
    auto& wasapi = item["wasapi"];
    wasapi["stream_initialized"] = false;
    wasapi["ks_format_support"] = "not_inferred";
    if (state == DEVICE_STATE_ACTIVE) {
        ComPtr<IAudioClient> client;
        if (checked(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())), "Activate IAudioClient", errors)) {
            WAVEFORMATEX* raw_wave = nullptr;
            const HRESULT hr = client->GetMixFormat(&raw_wave);
            CoMemory<WAVEFORMATEX> wave(raw_wave);
            if (checked(hr, "GetMixFormat", errors) && wave) {
                wasapi["shared_mix_format"] = wave_format(*wave);
                // A query only: no Initialize, Start, volume, or default-device changes.
                const HRESULT supported = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wave.get(), nullptr);
                wasapi["mix_format_exclusive_query"] = supported == S_OK ? "supported" :
                    supported == AUDCLNT_E_UNSUPPORTED_FORMAT ? "unsupported_format" : "query_failed";
                if (FAILED(supported)) errors.push(error("IsFormatSupported exclusive mix format",
                    static_cast<DWORD>(supported), "hresult"));
            }
            REFERENCE_TIME default_period = 0, minimum_period = 0;
            if (checked(client->GetDevicePeriod(&default_period, &minimum_period), "GetDevicePeriod", errors)) {
                wasapi["default_period_100ns"] = static_cast<std::int64_t>(default_period);
                wasapi["minimum_period_100ns"] = static_cast<std::int64_t>(minimum_period);
                wasapi["period_evidence"] = "WASAPI query; not a negotiated KS period";
            }
        }
    } else wasapi["query_status"] = "not_attempted_inactive_endpoint";
    item["property_queries"] = errors.items().empty() ? "succeeded" : "partial_or_failed";
    item["errors"] = std::move(errors);
    return item;
}
}

Json enumerate_mmdevices() {
    auto result = Json::object(); auto errors = Json::array();
    result["endpoints"] = Json::array(); result["defaults"] = Json::array();
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (checked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)), "CoCreateInstance MMDeviceEnumerator", errors)) {
        for (auto flow : {eCapture, eRender}) for (auto role : {eConsole, eMultimedia, eCommunications}) {
            auto entry = Json::object();
            entry["direction"] = flow == eCapture ? "to_daw" : "from_daw";
            entry["role"] = static_cast<int>(role);
            ComPtr<IMMDevice> device;
            const HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, role, &device);
            if (SUCCEEDED(hr)) {
                LPWSTR raw = nullptr;
                const HRESULT id_hr = device->GetId(&raw);
                CoMemory<wchar_t> id(raw);
                if (checked(id_hr, "default GetId", errors)) entry["id"] = utf8(id.get());
            } else entry["error"] = error("GetDefaultAudioEndpoint", static_cast<DWORD>(hr), "hresult");
            result["defaults"].push(std::move(entry));
        }
        ComPtr<IMMDeviceCollection> devices;
        if (checked(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &devices), "EnumAudioEndpoints", errors)) {
            UINT count = 0;
            if (checked(devices->GetCount(&count), "GetCount", errors)) {
                for (UINT index = 0; index < count; ++index) {
                    ComPtr<IMMDevice> device;
                    if (checked(devices->Item(index, &device), "IMMDeviceCollection::Item", errors))
                        result["endpoints"].push(endpoint(device.Get(), enumerator.Get()));
                }
            }
        }
    }
    result["errors"] = std::move(errors);
    return result;
}

Json correlate(const Json& endpoints, const Json& filters) {
    auto matches = Json::array();
    for (const auto& ep : endpoints.at("endpoints").items()) {
        if (!ep.at("id").string_if()) continue;
        for (const auto& connected : ep.at("connected_adapters").items()) {
            const auto* adapter_instance = connected.at("device_instance_id").string_if();
            if (!adapter_instance) continue;
            for (const auto& filter : filters.at("filters").items()) {
                const auto* filter_instance = filter.at("device_instance_id").string_if();
                if (!filter_instance || !same_windows_id(*adapter_instance, *filter_instance)) continue;
                auto match = Json::object();
                match["endpoint_id"] = ep.at("id"); match["ks_path"] = filter.at("path");
                match["device_instance_id"] = *filter_instance;
                match["evidence"] = "connected_adapter_and_KS_share_PnP_instance";
                match["exact_pin_mapping"] = "unverified";
                match["shared_clock_verified"] = false;
                match["use"] = "same device group; resolve pin mapping before opening through multiple APIs";
                matches.push(std::move(match));
            }
        }
    }
    return matches;
}
}
