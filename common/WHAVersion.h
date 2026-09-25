#pragma once

// WHAVersion - product name, version and credit, in one place for the C++ code (Control Panel ABOUT)
// and the resource compiler (each binary's VERSIONINFO, common/WHAVersionInfo.rc). Plain #defines
// only: rc.exe reads this file too. Keep WHA_VERSION_* in step with CMakeLists.txt project(VERSION)
// and installer\build-installer.ps1 -Version.

#define WHA_PRODUCT_NAME "WinHookAudio"
#define WHA_AUTHOR "LogicCuteGuy"
#define WHA_COPYRIGHT "Copyright (c) 2026 LogicCuteGuy"
#define WHA_LICENSE "MIT License"
#define WHA_URL "https://github.com/LogicCuteGuy/WinHookAudio"

#define WHA_VERSION_MAJOR 0
#define WHA_VERSION_MINOR 1
#define WHA_VERSION_PATCH 2
#define WHA_VERSION_STRING "0.1.2"
