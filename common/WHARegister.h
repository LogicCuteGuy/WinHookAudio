#pragma once

// WHARegister — DllRegisterServer / DllUnregisterServer for the ASIO drivers (regsvr32, elevated).
// Vocabulary: Master Driver, Bridge Driver.
// Writes what an ASIO host needs to list and load a driver (64-bit view, HKLM):
//   SOFTWARE\Classes\CLSID\{clsid}                 (default) = name
//   SOFTWARE\Classes\CLSID\{clsid}\InprocServer32  (default) = this DLL, ThreadingModel = Apartment
//   SOFTWARE\ASIO\<name>                           CLSID = {clsid}, Description
// Same keys the installer writes (installer/WinHookAudio.iss).

#include <windows.h>
#include <objbase.h>
#include <olectl.h>

#include <string>

namespace wha {

struct AsioRegistration {
  const GUID* clsid;
  const wchar_t* name;         // SOFTWARE\ASIO subkey, shown in the DAW's driver list
  const wchar_t* description;
};

inline std::wstring GuidString(const GUID& g) {
  wchar_t buf[40] = {};
  StringFromGUID2(g, buf, 40);
  return buf;
}

inline LONG SetRegString(const std::wstring& subKey, const wchar_t* valueName, const std::wstring& data) {
  HKEY key = nullptr;
  LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
  if (r != ERROR_SUCCESS) return r;
  r = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                     static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return r;
}

inline std::wstring ClsidKey(const GUID& clsid) { return L"SOFTWARE\\Classes\\CLSID\\" + GuidString(clsid); }
inline std::wstring AsioKey(const wchar_t* name) { return std::wstring(L"SOFTWARE\\ASIO\\") + name; }

// Unregister first on failure so a half-written registration never lists a driver that cannot load.
inline HRESULT UnregisterAsioDrivers(const AsioRegistration* regs, size_t count) {
  HRESULT hr = S_OK;
  for (size_t i = 0; i < count; ++i) {
    for (const std::wstring& key : {ClsidKey(*regs[i].clsid), AsioKey(regs[i].name)}) {
      const LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
      if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) hr = HRESULT_FROM_WIN32(r);
    }
  }
  return hr;
}

inline HRESULT RegisterAsioDrivers(HMODULE module, const AsioRegistration* regs, size_t count) {
  wchar_t path[MAX_PATH] = {};
  const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return SELFREG_E_CLASS;
  for (size_t i = 0; i < count; ++i) {
    const std::wstring clsidKey = ClsidKey(*regs[i].clsid);
    const std::wstring asioKey = AsioKey(regs[i].name);
    LONG r = SetRegString(clsidKey, nullptr, regs[i].name);
    if (r == ERROR_SUCCESS) r = SetRegString(clsidKey + L"\\InprocServer32", nullptr, path);
    if (r == ERROR_SUCCESS) r = SetRegString(clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    if (r == ERROR_SUCCESS) r = SetRegString(asioKey, L"CLSID", GuidString(*regs[i].clsid));
    if (r == ERROR_SUCCESS) r = SetRegString(asioKey, L"Description", regs[i].description);
    if (r != ERROR_SUCCESS) {
      UnregisterAsioDrivers(regs, count);
      return HRESULT_FROM_WIN32(r);  // E_ACCESSDENIED when regsvr32 is not elevated
    }
  }
  return S_OK;
}

}  // namespace wha
