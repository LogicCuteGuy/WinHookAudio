#pragma once

// WHAInstaller — elevated installer checks for 20.
// Vocabulary: Control Panel, Virtual Cable, Network Stream.

#include <string>
#include <cstdint>

namespace wha {

bool IsAdmin();
bool CheckInnoSetup();  // iscc exists
bool CheckPnputil();    // pnputil exists
bool CheckNetsh();      // netsh exists
bool CheckBcdedit();    // bcdedit exists
bool CheckEVCodeSigning();  // EV Cert placeholder
bool CheckAttestation();    // Microsoft Attestation placeholder

struct InstallerStatus {
  bool isAdmin = false;
  bool hasInno = false;
  bool hasPnputil = false;
  bool hasNetsh = false;
  bool hasBcdedit = false;
  bool hasEVCert = false;
  bool hasAttestation = false;
  bool canInstallDriver = false;  // ADMIN + pnputil + bcdedit
  bool canInstallFirewall = false;  // ADMIN + netsh
  bool canBuildSetup = false;  // hasInno
};

InstallerStatus GetInstallerStatus();
std::string InstallerStatusJson(const InstallerStatus& status);

}  // namespace wha
