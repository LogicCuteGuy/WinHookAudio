#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "WHAAsio.h"
#include "WHARegister.h"

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  // Files in the source tree (WHA_SOURCE_DIR from CMake, so the build folder can be anywhere).
  auto readFile = [](const char* path) -> std::string {
    std::ifstream f(std::string(WHA_SOURCE_DIR) + "/" + path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  const std::string regContent = readFile("installer/WinHookAudio.reg");
  check("reg has Master", regContent.find("WinHookAudio Master") != std::string::npos);
  check("reg has Bridge 1", regContent.find("Bridge 1") != std::string::npos);
  check("reg has Bridge 4", regContent.find("Bridge 4") != std::string::npos);
  // The 5 CLSIDs the drivers answer to must be exactly the ones the installer registers.
  const GUID* clsids[] = {&wha::CLSID_WinHookMaster, &wha::CLSID_WinHookBridge1, &wha::CLSID_WinHookBridge2,
                          &wha::CLSID_WinHookBridge3, &wha::CLSID_WinHookBridge4};
  const std::string issForGuids = readFile("installer/WinHookAudio.iss");
  bool regMatch = true, issAsio = true, issCom = true, notPlaceholder = true;
  for (const GUID* g : clsids) {
    const std::wstring w = wha::GuidString(*g);  // {XXXXXXXX-...}
    std::string braced;
    for (wchar_t c : w) braced.push_back(static_cast<char>(c));  // GUID text is ASCII
    const std::string bare = braced.substr(1, 36);
    regMatch = regMatch && regContent.find("\"CLSID\"=\"" + braced + "\"") != std::string::npos;
    issAsio = issAsio && issForGuids.find("ValueName: \"CLSID\"; ValueData: \"{{" + bare + "}\"") != std::string::npos;
    issCom = issCom && issForGuids.find("SOFTWARE\\Classes\\CLSID\\{{" + bare + "}\\InprocServer32\"; ValueType: string; ValueName: \"\"; ValueData: \"{app}\\") != std::string::npos;
    notPlaceholder = notPlaceholder && bare.rfind("12345678-1234", 0) != 0;
  }
  check("CLSIDs are real GUIDs, not placeholders", notPlaceholder);
  check("reg CLSIDs = driver CLSIDs (5)", regMatch);
  check("iss SOFTWARE\\ASIO CLSIDs = driver CLSIDs, {{ escaped", issAsio);
  check("iss registers COM InprocServer32 for all 5 CLSIDs", issCom);

  const std::string issContent = issForGuids;
  check("iss has [Setup]", issContent.find("[Setup]") != std::string::npos);
  check("iss has [Files]", issContent.find("[Files]") != std::string::npos);
  check("iss has [Registry]", issContent.find("[Registry]") != std::string::npos);
  check("iss has [Run]", issContent.find("[Run]") != std::string::npos);
  check("iss has LicenseFile", issContent.find("LicenseFile=..\\LICENSE") != std::string::npos);
  check("iss offers signed / not-signed Virtual Cable as exclusive choices",
        issContent.find("Name: \"cable\\signed\"") != std::string::npos &&
            issContent.find("Name: \"cable\\test\"") != std::string::npos &&
            issContent.find("Flags: exclusive") != std::string::npos);
  check("iss creates / removes the Root device with winhookaudio-devsetup",
        issContent.find("'install \"' + Inf") != std::string::npos &&
            issContent.find("winhookaudio-devsetup.exe\"; Parameters: \"remove\"") != std::string::npos);
  check("iss test mode only for the test-signed choice",
        issContent.find("/set testsigning on") != std::string::npos &&
            issContent.find("if WizardIsComponentSelected('cable\\test') then") != std::string::npos);
  check("iss has netsh", issContent.find("netsh") != std::string::npos);
  check("iss has WinHookAudioMasterASIO64", issContent.find("WinHookAudioMasterASIO64") != std::string::npos);

  const std::string infContent = readFile("driver/WinHookAudio.inf");
  check("inf has Root\\WinHookAudio", infContent.find("Root\\WinHookAudio") != std::string::npos);
  check("inf has WinHookAudio.sys", infContent.find("WinHookAudio.sys") != std::string::npos);

  std::printf("{\"schema_version\":1,\"operation\":\"installer_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
