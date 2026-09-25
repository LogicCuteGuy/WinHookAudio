// arm64x-test — loads an ASIO DLL the way COM loads an InprocServer32 path (LoadLibraryEx with
// LOAD_WITH_ALTERED_SEARCH_PATH, so the DLL's own folder is searched for what it loads), asks it for
// the class factory of a driver's CLSID and says which module the factory's code is in. Run against
// the ARM64X forwarder (ADR 0016) from an ARM64 and an x64 process on Windows on ARM: each must get a
// factory from its own architecture's DLL.
//
// usage: arm64x-test <dll> <CLSID {...}> <module the factory must come from, e.g. WinHookAudioMasterASIOARM64.dll>

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

#include <cstdio>
#include <cwchar>
#include <initializer_list>

#pragma comment(lib, "ole32.lib")

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) return std::printf("usage: arm64x-test <dll> <CLSID> <expected module>\n"), 2;
  CLSID clsid{};
  if (FAILED(CLSIDFromString(argv[2], &clsid))) return std::printf("bad CLSID: FAIL\n"), 2;
  HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) return std::printf("LoadLibraryEx failed %lu: FAIL\n", GetLastError()), 1;
  using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
  auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
  bool pass = getClassObject != nullptr;
  for (const char* name : {"DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer"}) pass = pass && GetProcAddress(dll, name);
  std::printf("  exports: %s\n", pass ? "PASS" : "FAIL");
  IClassFactory* factory = nullptr;
  const HRESULT hr = getClassObject ? getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory)) : E_FAIL;
  wchar_t path[MAX_PATH] = L"(none)";
  if (SUCCEEDED(hr) && factory) {
    void* code = (*reinterpret_cast<void***>(factory))[0];  // the factory's first method
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(code), &owner))
      GetModuleFileNameW(owner, path, MAX_PATH);
    factory->Release();
  }
  const wchar_t* file = std::wcsrchr(path, L'\\');
  file = file ? file + 1 : path;
  const bool fromExpected = SUCCEEDED(hr) && _wcsicmp(file, argv[3]) == 0;
  std::printf("  class factory: 0x%08lX from %ls: %s\n", static_cast<unsigned long>(hr), file, fromExpected ? "PASS" : "FAIL");
  pass = pass && fromExpected;
  std::printf("{\"schema_version\":1,\"operation\":\"arm64x_test\",\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
