// arm64x-test — loads an ASIO DLL the way COM loads an InprocServer32 path (LoadLibraryEx with
// LOAD_WITH_ALTERED_SEARCH_PATH, so the DLL's own folder is searched for what it needs) and says
// which module its exports run in. Run against the ARM64X forwarder (ADR 0016) from an ARM64 and an
// x64 process on Windows on ARM: each must land in its own architecture's DLL.
//
// usage: arm64x-test <dll> <module name every export must be in, e.g. WinHookAudioMasterASIOARM64.dll>

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <initializer_list>

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) return std::printf("usage: arm64x-test <dll> <expected module>\n"), 2;
  HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) return std::printf("LoadLibraryEx failed %lu: FAIL\n", GetLastError()), 1;
  bool pass = true;
  for (const char* name : {"DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer"}) {
    FARPROC f = GetProcAddress(dll, name);
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH] = L"(none)";
    if (f && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(f), &owner))
      GetModuleFileNameW(owner, path, MAX_PATH);
    const wchar_t* file = std::wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    const bool ok = f && _wcsicmp(file, argv[2]) == 0;
    std::printf("  %s -> %ls: %s\n", name, file, ok ? "PASS" : "FAIL");
    pass = pass && ok;
  }
  std::printf("{\"schema_version\":1,\"operation\":\"arm64x_test\",\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
