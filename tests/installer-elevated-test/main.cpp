#include <cstdio>
#include "WHAInstaller.h"

using namespace wha;

int main() {
  bool pass = true;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) pass = false;
  };

  InstallerStatus s = GetInstallerStatus();
  std::string json = InstallerStatusJson(s);
  check("status json not empty", !json.empty());
  check("status has isAdmin", json.find("isAdmin") != std::string::npos);
  check("status has hasInno", json.find("hasInno") != std::string::npos);
  check("status has canInstallDriver", json.find("canInstallDriver") != std::string::npos);
  check("status has canBuildSetup", json.find("canBuildSetup") != std::string::npos);
  check("isAdmin is bool", s.isAdmin == false || s.isAdmin == true);
  check("hasPnputil true", s.hasPnputil == true);
  check("hasNetsh true", s.hasNetsh == true);
  check("hasEVCert false offline", s.hasEVCert == false);
  check("hasAttestation false offline", s.hasAttestation == false);

  std::printf("{\"schema_version\":1,\"operation\":\"installer_elevated_test\",\"stream_verified\":false,\"pass\":%s}\n", pass ? "true" : "false");
  return pass ? 0 : 1;
}
