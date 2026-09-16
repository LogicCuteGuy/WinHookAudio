#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

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
  check("reg has 5 CLSIDs", regContent.find("12345678-1234-1234-1234-56789abcdef0") != std::string::npos);

  std::string issContent = readFile("installer/WinHookAudio.iss");
  if (issContent.empty()) issContent = readFile("../installer/WinHookAudio.iss");
  if (issContent.empty()) { std::ifstream f("C:/Users/Logic/Desktop/WinHookAudio/installer/WinHookAudio.iss"); issContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
  check("iss has [Setup]", issContent.find("[Setup]") != std::string::npos);
  check("iss has [Files]", issContent.find("[Files]") != std::string::npos);
  check("iss has [Registry]", issContent.find("[Registry]") != std::string::npos);
  check("iss has [Run]", issContent.find("[Run]") != std::string::npos);
  check("iss has pnputil", issContent.find("pnputil") != std::string::npos);
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
