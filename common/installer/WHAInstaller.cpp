#include "WHAInstaller.h"
#include <windows.h>
#include <string>
#include <memory>

namespace wha {

bool IsAdmin() {
  BOOL isAdmin = FALSE;
  PSID adminGroup = nullptr;
  SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
  if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
    struct SidDeleter { void operator()(PSID p) const { if (p) FreeSid(p); } };
    std::unique_ptr<void, SidDeleter> guard(adminGroup);
    CheckTokenMembership(nullptr, adminGroup, &isAdmin);
  }
  return isAdmin != FALSE;
}

bool CheckInnoSetup() {
  // Check if iscc exists in PATH or common locations
  return false;  // offline: not installed
}

bool CheckPnputil() {
  // pnputil is inbox Win10+
  return true;
}

bool CheckNetsh() {
  return true;  // inbox
}

bool CheckBcdedit() {
  return true;  // inbox
}

bool CheckEVCodeSigning() {
  return false;  // placeholder — requires EV Cert procurement
}

bool CheckAttestation() {
  return false;  // placeholder — requires Microsoft Attestation
}

InstallerStatus GetInstallerStatus() {
  InstallerStatus s;
  s.isAdmin = IsAdmin();
  s.hasInno = CheckInnoSetup();
  s.hasPnputil = CheckPnputil();
  s.hasNetsh = CheckNetsh();
  s.hasBcdedit = CheckBcdedit();
  s.hasEVCert = CheckEVCodeSigning();
  s.hasAttestation = CheckAttestation();
  s.canInstallDriver = s.isAdmin && s.hasPnputil && s.hasBcdedit;
  s.canInstallFirewall = s.isAdmin && s.hasNetsh;
  s.canBuildSetup = s.hasInno;
  return s;
}

std::string InstallerStatusJson(const InstallerStatus& s) {
  std::string json = "{";
  json += "\"isAdmin\":" + std::string(s.isAdmin ? "true" : "false") + ",";
  json += "\"hasInno\":" + std::string(s.hasInno ? "true" : "false") + ",";
  json += "\"hasPnputil\":" + std::string(s.hasPnputil ? "true" : "false") + ",";
  json += "\"hasNetsh\":" + std::string(s.hasNetsh ? "true" : "false") + ",";
  json += "\"hasBcdedit\":" + std::string(s.hasBcdedit ? "true" : "false") + ",";
  json += "\"hasEVCert\":" + std::string(s.hasEVCert ? "true" : "false") + ",";
  json += "\"hasAttestation\":" + std::string(s.hasAttestation ? "true" : "false") + ",";
  json += "\"canInstallDriver\":" + std::string(s.canInstallDriver ? "true" : "false") + ",";
  json += "\"canInstallFirewall\":" + std::string(s.canInstallFirewall ? "true" : "false") + ",";
  json += "\"canBuildSetup\":" + std::string(s.canBuildSetup ? "true" : "false");
  json += "}";
  return json;
}

}  // namespace wha
