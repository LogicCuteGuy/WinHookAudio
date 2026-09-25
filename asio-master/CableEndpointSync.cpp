#include "CableEndpointSync.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cwchar>
#include <cwctype>

#include "WHASlotTable.h"
#include "virtual/WHACableFormat.h"

namespace wha {

namespace {

// IPolicyConfig (Windows 7 and later; undocumented, the interface Sound settings uses).
enum PolicyShareMode { kPolicyShared, kPolicyExclusive };
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, PolicyShareMode*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, PolicyShareMode*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// Endpoint properties: the device it belongs to (Root\WinHookAudio = ours), its KS filter's interface
// path (ends in \outputwaveN or \inputwaveN: cable N), and the format the audio engine opens it in.
constexpr PROPERTYKEY kMatchingDeviceId = {{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 8};
constexpr PROPERTYKEY kFilterPath = {{0x233164c8, 0x1b2c, 0x4c7d, {0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67}}, 1};
constexpr PROPERTYKEY kDeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

constexpr int kCables = kVirtualSlotCables;
constexpr DWORD kPassMs = 2000;          // a pass without a wake
constexpr ULONGLONG kRetryMs = 10000;   // the same endpoint, same wanted format, after a failure

// Cable (0-based) of one of our endpoints, from its filter path; -1 if not one.
int CableOfFilterPath(const wchar_t* path, bool capture) {
  if (!path) return -1;
  const wchar_t* tail = std::wcsrchr(path, L'\\');
  tail = tail ? tail + 1 : path;
  const wchar_t* prefix = capture ? L"inputwave" : L"outputwave";
  const size_t n = std::wcslen(prefix);
  if (_wcsnicmp(tail, prefix, n) != 0 || !std::iswdigit(tail[n])) return -1;
  int number = 0;
  for (const wchar_t* d = tail + n; *d; ++d) {  // "outputwave1" .. "outputwave16"
    if (!std::iswdigit(*d) || number > kCables) return -1;
    number = number * 10 + (*d - L'0');
  }
  return number >= 1 && number <= kCables ? number - 1 : -1;
}

struct Wanted {
  uint32_t rate = 0, channels = 0, format = 0;
  bool operator==(const Wanted&) const = default;
};

WAVEFORMATEXTENSIBLE MakeFormat(const Wanted& w, bool mix) {
  const auto kind = static_cast<WHASampleKind>(w.format);
  const WORD bits = static_cast<WORD>(mix ? 32 : SampleBytes(kind) * 8);
  WAVEFORMATEXTENSIBLE x{};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = static_cast<WORD>(w.channels);
  x.Format.nSamplesPerSec = w.rate;
  x.Format.wBitsPerSample = bits;
  x.Format.nBlockAlign = static_cast<WORD>(w.channels * bits / 8);
  x.Format.nAvgBytesPerSec = w.rate * x.Format.nBlockAlign;
  x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  x.Samples.wValidBitsPerSample = static_cast<WORD>(mix ? 32 : ValidBits(kind));
  x.dwChannelMask = CableChannelMask(w.channels);
  x.SubFormat = mix || kind == WHASampleKind::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return x;
}

// The endpoint's device format (a WAVEFORMATEX blob) already is `w`.
bool DeviceFormatIs(IPropertyStore* props, const Wanted& w) {
  PROPVARIANT v;
  PropVariantInit(&v);
  bool same = false;
  if (SUCCEEDED(props->GetValue(kDeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
    const auto* f = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
    const auto kind = static_cast<WHASampleKind>(w.format);
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    unsigned valid = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && v.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
      const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
      isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      valid = x->Samples.wValidBitsPerSample;
    }
    same = f->nSamplesPerSec == w.rate && f->nChannels == w.channels && f->wBitsPerSample == SampleBytes(kind) * 8 &&
           valid == ValidBits(kind) && isFloat == (kind == WHASampleKind::Float32);
  }
  PropVariantClear(&v);
  return same;
}

bool StringProperty(IPropertyStore* props, const PROPERTYKEY& key, std::wstring& out) {
  PROPVARIANT v;
  PropVariantInit(&v);
  const bool ok = SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal;
  if (ok) out = v.pwszVal;
  PropVariantClear(&v);
  return ok;
}

}  // namespace

bool CableEndpointSync::start() {
  if (thread_) return true;
  stop_ = false;
  wake_ = CreateEventW(nullptr, FALSE, TRUE, nullptr);  // signalled: the first pass runs at once
  thread_ = wake_ ? CreateThread(nullptr, 0, threadProc, this, 0, nullptr) : nullptr;
  return thread_ != nullptr;
}

void CableEndpointSync::stop() {
  if (thread_) {
    stop_ = true;
    SetEvent(wake_);
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (wake_) CloseHandle(wake_);
  wake_ = nullptr;
}

DWORD WINAPI CableEndpointSync::threadProc(LPVOID self) {
  static_cast<CableEndpointSync*>(self)->run();
  return 0;
}

void CableEndpointSync::run() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  IPolicyConfig* policy = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), reinterpret_cast<void**>(&policy));
  if (!policy) lastError_ = static_cast<int32_t>(REGDB_E_CLASSNOTREG);
  Wanted tried[kCables][2] = {};           // the format last set on each endpoint...
  ULONGLONG triedAt[kCables][2] = {};      // ...and when
  while (!stop_.load() && enumerator && policy) {
    WaitForSingleObject(wake_, kPassMs);
    if (stop_.load()) break;
    for (int dir = 0; dir < 2; ++dir) {
      const bool capture = dir == 1;
      IMMDeviceCollection* list = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &list))) continue;
      UINT count = 0;
      list->GetCount(&count);
      for (UINT i = 0; i < count && !stop_.load(); ++i) {
        IMMDevice* device = nullptr;
        IPropertyStore* props = nullptr;
        LPWSTR id = nullptr;
        if (SUCCEEDED(list->Item(i, &device)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
            SUCCEEDED(device->GetId(&id))) {
          std::wstring owner, path;
          const int cable = StringProperty(props, kMatchingDeviceId, owner) && _wcsicmp(owner.c_str(), L"Root\\WinHookAudio") == 0 &&
                                    StringProperty(props, kFilterPath, path)
                                ? CableOfFilterPath(path.c_str(), capture)
                                : -1;
          Wanted want;
          if (cable >= 0 && driverFormat_(cable, want.rate, want.channels, want.format) && want.rate &&
              IsValidCableChannels(want.channels) && IsValidCableFormat(want.format) && !DeviceFormatIs(props, want)) {
            const ULONGLONG now = GetTickCount64();
            if (!(tried[cable][dir] == want) || now - triedAt[cable][dir] >= kRetryMs) {
              tried[cable][dir] = want;
              triedAt[cable][dir] = now;
              WAVEFORMATEXTENSIBLE endpoint = MakeFormat(want, false);
              WAVEFORMATEXTENSIBLE mix = MakeFormat(want, true);
              const HRESULT hr = policy->SetDeviceFormat(id, &endpoint.Format, &mix.Format);
              if (SUCCEEDED(hr)) switches_.fetch_add(1);
              else lastError_ = static_cast<int32_t>(hr);
            }
          }
        }
        if (id) CoTaskMemFree(id);
        if (props) props->Release();
        if (device) device->Release();
      }
      list->Release();
    }
  }
  if (policy) policy->Release();
  if (enumerator) enumerator->Release();
  if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace wha
