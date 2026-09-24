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

  // Check WinHookAudio.reg has 5 CLSIDs (try build dir and source dir)
  auto readFile = [](const char* path) -> std::string {
    std::ifstream f(path);
    if (!f) { std::ifstream f2(std::string("../") + path); if (f2) return std::string((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>()); return ""; }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  std::string regContent = readFile("installer/WinHookAudio.reg");
  if (regContent.empty()) regContent = readFile("../installer/WinHookAudio.reg");
  // Also try absolute
  if (regContent.empty()) { std::ifstream f("C:/Users/Logic/Desktop/WinHookAudio/installer/WinHookAudio.reg"); regContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
  check("reg has Master", regContent.find("WinHookAudio Master") != std::string::npos);
  check("reg has Bridge 1", regContent.find("Bridge 1") != std::string::npos);
  check("reg has Bridge 4", regContent.find("Bridge 4") != std::string::npos);
  // The 5 CLSIDs the drivers answer to must be exactly the ones the installer registers.
  const GUID* clsids[] = {&wha::CLSID_WinHookMaster, &wha::CLSID_WinHookBridge1, &wha::CLSID_WinHookBridge2,
                          &wha::CLSID_WinHookBridge3, &wha::CLSID_WinHookBridge4};
  std::string issForGuids = readFile("installer/WinHookAudio.iss");
  if (issForGuids.empty()) issForGuids = readFile("../installer/WinHookAudio.iss");
  if (issForGuids.empty()) { std::ifstream f("C:/Users/Logic/Desktop/WinHookAudio/installer/WinHookAudio.iss"); issForGuids = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
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

  std::string issContent = readFile("installer/WinHookAudio.iss");
  if (issContent.empty()) issContent = readFile("../installer/WinHookAudio.iss");
  if (issContent.empty()) { std::ifstream f("C:/Users/Logic/Desktop/WinHookAudio/installer/WinHookAudio.iss"); issContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
  check("iss has [Setup]", issContent.find("[Setup]") != std::string::npos);
  check("iss has [Files]", issContent.find("[Files]") != std::string::npos);
  check("iss has [Registry]", issContent.find("[Registry]") != std::string::npos);
  check("iss has [Run]", issContent.find("[Run]") != std::string::npos);
  check("iss has pnputil", issContent.find("pnputil") != std::string::npos);
  check("iss runs pnputil only when the Virtual Cable driver is included", issContent.find("Check: VirtualCableIncluded") != std::string::npos);
  check("iss test-signing is an unchecked opt-in task", issContent.find("Name: \"testsigning\"") != std::string::npos &&
                                                             issContent.find("Tasks: testsigning") != std::string::npos);
  check("iss has netsh", issContent.find("netsh") != std::string::npos);
  check("iss has WinHookAudioMasterASIO64", issContent.find("WinHookAudioMasterASIO64") != std::string::npos);

  std::string infContent = readFile("installer/WinHookAudio.inf");
  if (infContent.empty()) infContent = readFile("../installer/WinHookAudio.inf");
  if (infContent.empty()) { std::ifstream f("C:/Users/Logic/Desktop/WinHookAudio/installer/WinHookAudio.inf"); infContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
  check("inf has Root\\WinHookAudio", infContent.find("Root\\WinHookAudio") != std::string::npos);
  check("inf has WinHookAudio.sys", infContent.find("WinHookAudio.sys") != std::string::npos);

  std::string catContent = readFile("installer/WinHookAudio.cat");
  if (catContent.empty()) catContent = readFile("../installer/WinHookAudio.cat");
  if (catContent.empty()) { std::ifstream f("C:/Users/Logic/Desktop/WinHookAudio/installer/WinHookAudio.cat"); catContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
  check("cat exists", !catContent.empty());

  std::printf("{\"schema_version\":1,\"operation\":\"installer_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
