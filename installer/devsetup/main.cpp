// winhookaudio-devsetup - creates, updates and removes the Root\WinHookAudio device (Virtual Cable
// driver) for the installer. pnputil /add-driver only stages a package: a root-enumerated device
// needs its device node created first (what devcon install does), and devcon is not redistributable.
//
//   winhookaudio-devsetup install <path\WinHookAudio.inf>   create the device if missing, install the driver
//   winhookaudio-devsetup remove                            remove the device and every WinHookAudio.inf package
//   winhookaudio-devsetup status                            print the device and driver state (changes nothing)
//
// Exit: 0 ok, 3010 ok but Windows wants a restart, 1 failed (message on stderr), 2 bad arguments.

#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kHardwareId[] = L"Root\\WinHookAudio";
constexpr wchar_t kInfName[] = L"winhookaudio.inf";
constexpr int kExitRestart = 3010;

int Fail(const char* what, DWORD error = GetLastError()) {
  std::fprintf(stderr, "winhookaudio-devsetup: %s failed (error %lu / 0x%08lX)\n", what, error, error);
  return 1;
}

// True when the device's hardware id list holds Root\WinHookAudio.
bool HasOurHardwareId(HDEVINFO set, SP_DEVINFO_DATA& device) {
  wchar_t ids[1024] = {};
  if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                         sizeof(ids) - sizeof(wchar_t) * 2, nullptr))
    return false;
  for (const wchar_t* id = ids; *id; id += wcslen(id) + 1)
    if (_wcsicmp(id, kHardwareId) == 0) return true;
  return false;
}

// Every device node with our hardware id, present or not.
int CountDevices(bool remove, bool* rebootNeeded) {
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) return -1;
  int count = 0;
  SP_DEVINFO_DATA device{sizeof(device)};
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &device); ++i) {
    if (!HasOurHardwareId(set, device)) continue;
    ++count;
    if (remove) {
      BOOL reboot = FALSE;
      if (!DiUninstallDevice(nullptr, set, &device, 0, &reboot)) Fail("DiUninstallDevice");
      if (reboot && rebootNeeded) *rebootNeeded = true;
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  return count;
}

// Creates the Root\WinHookAudio device node (class and class GUID from the INF).
int CreateDeviceNode(const std::wstring& inf) {
  GUID classGuid{};
  wchar_t className[MAX_CLASS_NAME_LEN] = {};
  if (!SetupDiGetINFClassW(inf.c_str(), &classGuid, className, MAX_CLASS_NAME_LEN, nullptr))
    return Fail("SetupDiGetINFClass");
  HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
  if (set == INVALID_HANDLE_VALUE) return Fail("SetupDiCreateDeviceInfoList");
  int result = 0;
  SP_DEVINFO_DATA device{sizeof(device)};
  const wchar_t ids[] = L"Root\\WinHookAudio\0";  // REG_MULTI_SZ: the literal adds the second NUL
  if (!SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &device))
    result = Fail("SetupDiCreateDeviceInfo");
  else if (!SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids),
                                              sizeof(ids)))
    result = Fail("SetupDiSetDeviceRegistryProperty");
  else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device))
    result = Fail("DIF_REGISTERDEVICE");
  SetupDiDestroyDeviceInfoList(set);
  return result;
}

int Install(const wchar_t* infArg) {
  wchar_t full[MAX_PATH] = {};
  if (!GetFullPathNameW(infArg, MAX_PATH, full, nullptr) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
    return Fail("finding the INF", ERROR_FILE_NOT_FOUND);
  const int existing = CountDevices(false, nullptr);
  if (existing < 0) return Fail("listing devices");
  if (existing == 0) {
    std::printf("Creating the Root\\WinHookAudio device\n");
    if (const int r = CreateDeviceNode(full)) return r;
  } else {
    std::printf("Updating the driver of the existing Root\\WinHookAudio device\n");
  }
  BOOL reboot = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, full, INSTALLFLAG_FORCE, &reboot))
    return Fail("UpdateDriverForPlugAndPlayDevices");
  std::printf("Driver installed%s\n", reboot ? " (restart Windows to finish)" : "");
  return reboot ? kExitRestart : 0;
}

// oemNN.inf files in %windir%\INF whose original name is WinHookAudio.inf.
std::vector<std::wstring> OurDriverPackages() {
  std::vector<std::wstring> result;
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  const std::wstring dir = std::wstring(windir) + L"\\INF\\";
  WIN32_FIND_DATAW found{};
  HANDLE find = FindFirstFileW((dir + L"oem*.inf").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE) return result;
  do {
    const std::wstring path = dir + found.cFileName;
    DWORD size = 0;
    if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &size) || size == 0) continue;
    std::vector<BYTE> buffer(size);
    auto* info = reinterpret_cast<SP_INF_INFORMATION*>(buffer.data());
    if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, info, size, nullptr)) continue;
    SP_ORIGINAL_FILE_INFO_W original{sizeof(original)};
    if (SetupQueryInfOriginalFileInformationW(info, 0, nullptr, &original) &&
        _wcsicmp(original.OriginalInfName, kInfName) == 0)
      result.push_back(found.cFileName);
  } while (FindNextFileW(find, &found));
  FindClose(find);
  return result;
}

int Remove() {
  bool reboot = false;
  const int devices = CountDevices(true, &reboot);
  if (devices < 0) return Fail("listing devices");
  std::printf("Removed %d Root\\WinHookAudio device(s)\n", devices);
  int result = 0;
  for (const std::wstring& package : OurDriverPackages()) {
    if (SetupUninstallOEMInfW(package.c_str(), SUOI_FORCEDELETE, nullptr))
      std::printf("Removed driver package %ls\n", package.c_str());
    else
      result = Fail("SetupUninstallOEMInf");
  }
  if (result == 0 && reboot) {
    std::printf("Restart Windows to finish\n");
    return kExitRestart;
  }
  return result;
}

const char* ServiceState() {
  const char* text = "not installed";
  if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
    if (SC_HANDLE svc = OpenServiceW(scm, L"WinHookAudio", SERVICE_QUERY_STATUS)) {
      SERVICE_STATUS st{};
      text = QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING ? "running" : "installed, not running";
      CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
  }
  return text;
}

int Status() {
  const int devices = CountDevices(false, nullptr);
  if (devices < 0) return Fail("listing devices");
  std::printf("Root\\WinHookAudio devices: %d\n", devices);
  std::printf("Driver packages:");
  for (const std::wstring& package : OurDriverPackages()) std::printf(" %ls", package.c_str());
  std::printf("\nWinHookAudio.sys: %s\n", ServiceState());
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && _wcsicmp(argv[1], L"install") == 0) return Install(argv[2]);
  if (argc == 2 && _wcsicmp(argv[1], L"remove") == 0) return Remove();
  if (argc == 2 && _wcsicmp(argv[1], L"status") == 0) return Status();
  std::fprintf(stderr, "usage: winhookaudio-devsetup install <WinHookAudio.inf> | remove | status\n");
  return 2;
}
