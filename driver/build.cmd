@echo off
rem build.cmd - builds WinHookAudio.sys and its test-signed driver package into build\driver\.
rem Needs Visual Studio 2022 or newer (MSVC x64) and the WDK; picks the newest installed of each
rem (this PC: VS 18 + WDK km 10.0.28000 + SDK shared 10.0.26100; GitHub windows-2025: VS 2022 + WDK 10.0.26100).
rem Signs with a self-signed "WinHookAudio Test" code-signing certificate in the current user's
rem store (created once); the machine must be in test-signing mode to load the driver.
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\build\driver
set OBJ=%ROOT%\build\driver-obj
set WK=C:\Program Files (x86)\Windows Kits\10
set CERTNAME=WinHookAudio Test

rem Newest kit version that has each part (versions sort by name: same length).
for /f "delims=" %%v in ('dir /b /ad /o-n "%WK%\Include"') do (
  if not defined KMVER if exist "%WK%\Include\%%v\km\portcls.h" set KMVER=%%v
  if not defined SHAREDVER if exist "%WK%\Include\%%v\shared\sdkddkver.h" set SHAREDVER=%%v
)
for /f "delims=" %%v in ('dir /b /ad /o-n "%WK%\bin"') do (
  if not defined TOOLVER if exist "%WK%\bin\%%v\x64\stampinf.exe" set TOOLVER=%%v
  if not defined SIGNVER if exist "%WK%\bin\%%v\x64\signtool.exe" set SIGNVER=%%v
)
if not defined KMVER (echo build.cmd: WDK km headers not found in Windows Kits & exit /b 1)
if not defined SHAREDVER (echo build.cmd: SDK shared headers not found & exit /b 1)
if not defined TOOLVER (echo build.cmd: WDK tools stampinf/Inf2Cat not found & exit /b 1)
if not defined SIGNVER (echo build.cmd: signtool not found & exit /b 1)
set KM=%WK%\Include\%KMVER%\km
set SHARED=%WK%\Include\%SHAREDVER%\shared
set LIBKM=%WK%\Lib\%KMVER%\km\x64
set BIN=%WK%\bin\%TOOLVER%
set SIGNTOOL=%WK%\bin\%SIGNVER%\x64\signtool.exe

for /f "usebackq delims=" %%p in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set VCVARS=%%p
if not defined VCVARS (echo build.cmd: Visual Studio with MSVC x64 not found & exit /b 1)
call "%VCVARS%" >nul 2>nul
where cl >nul 2>nul || (echo build.cmd: MSVC not found & exit /b 1)
echo build.cmd: WDK km %KMVER%, SDK shared %SHAREDVER%, tools %TOOLVER%
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OUT%" mkdir "%OUT%"

set CFLAGS=/nologo /c /kernel /GS /Gy /GR- /Zi /O2 /W4 /WX /Zp8 /std:c++17 /D_AMD64_ /DAMD64 /D_WIN64 ^
 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DPOOL_NX_OPTIN=1 /X ^
 /I"%KM%" /I"%KM%\crt" /I"%SHARED%" /Fd"%OBJ%\WinHookAudio.pdb"
cl %CFLAGS% /Fo"%OBJ%\\" "%~dp0WHAAdapter.cpp" "%~dp0WHAMiniports.cpp" || exit /b 1
link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:GsDriverEntry /NODEFAULTLIB /OPT:REF /OPT:ICF /DEBUG ^
 /PDB:"%OUT%\WinHookAudio.pdb" /OUT:"%OUT%\WinHookAudio.sys" "%OBJ%\WHAAdapter.obj" "%OBJ%\WHAMiniports.obj" ^
 "%LIBKM%\ntoskrnl.lib" "%LIBKM%\hal.lib" "%LIBKM%\portcls.lib" "%LIBKM%\stdunk.lib" "%LIBKM%\ks.lib" ^
 "%LIBKM%\wdmsec.lib" "%LIBKM%\libcntpr.lib" "%LIBKM%\BufferOverflowFastFailK.lib" || exit /b 1

copy /y "%~dp0WinHookAudio.inf" "%OUT%\WinHookAudio.inf" >nul || exit /b 1
"%BIN%\x64\stampinf.exe" -f "%OUT%\WinHookAudio.inf" -d * -a amd64 -v * >nul || exit /b 1

rem The test certificate: create once in CurrentUser\My, export its public part for install.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
 "$c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=%CERTNAME%' } | Select-Object -First 1;" ^
 "if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERTNAME%' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) };" ^
 "Export-Certificate -Cert $c -FilePath '%OUT%\WinHookAudioTest.cer' | Out-Null" || exit /b 1

"%SIGNTOOL%" sign /q /fd SHA256 /s My /n "%CERTNAME%" "%OUT%\WinHookAudio.sys" || exit /b 1
if exist "%OUT%\WinHookAudio.cat" del "%OUT%\WinHookAudio.cat"
"%BIN%\x86\Inf2Cat.exe" /driver:"%OUT%" /os:10_X64 /uselocaltime >nul || (echo build.cmd: Inf2Cat failed & exit /b 1)
"%SIGNTOOL%" sign /q /fd SHA256 /s My /n "%CERTNAME%" "%OUT%\WinHookAudio.cat" || exit /b 1
echo build.cmd: %OUT%\WinHookAudio.sys + .inf + .cat (test-signed)
