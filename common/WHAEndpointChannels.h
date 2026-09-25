#pragma once

// WHAEndpointChannels — how many channels a Windows audio endpoint has: its device format (Windows
// Sound settings > the device > Advanced), the format HW slots open it with in exclusive mode. The
// Worker (KsEndpoint) and the Control Panel's Source menu both read it here, so they agree.
// Vocabulary: Slot.

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>

namespace wha {

// PKEY_AudioEngine_DeviceFormat (mmdeviceapi.h), defined here so no GUID library is needed.
constexpr PROPERTYKEY kEndpointDeviceFormatKey = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

// The endpoint's channel count (0 = unknown) and, when it says, its speaker mask (0 = none given).
inline int EndpointChannels(IPropertyStore* props, DWORD* mask = nullptr) {
  if (mask) *mask = 0;
  if (!props) return 0;
  int channels = 0;
  PROPVARIANT v;
  PropVariantInit(&v);
  if (SUCCEEDED(props->GetValue(kEndpointDeviceFormatKey, &v)) && v.vt == VT_BLOB && v.blob.pBlobData &&
      v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
    const auto* f = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
    channels = f->nChannels;
    if (mask && f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && v.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE))
      *mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->dwChannelMask;
  }
  PropVariantClear(&v);
  return channels;
}

}  // namespace wha
