; WinHookAudio Installer - Inno Setup 6.3+ (build with installer\build-installer.ps1)
; ASIO: 5 CLSIDs (Master + Bridge 1-4). Virtual Cable: WinHookAudio.sys, chosen on the Components page:
;   cable\signed  Microsoft-signed driver package (normal Windows), only when installer\driver-signed\ has one
;   cable\test    test-signed package from driver\build.cmd: trusts the test certificate, turns on Test Mode
; Firewall: UDP 6980-6981 (WHAA network stream).
; Architectures (ADR 0016): x64 DLLs always; 32-bit DLLs (build-x86) for 32-bit DAWs, in the 32-bit
; registry view; on Windows on ARM the ARM64 DLLs (build-arm64) and ARM64X forwarders, which the
; 64-bit view points at so ARM64 and x64 DAWs each get their own. The Virtual Cable driver is x64 only.

#ifndef AppVersion
  #define AppVersion "0.1.1"
#endif
#ifndef BuildDir
  #define BuildDir AddBackslash(SourcePath) + "..\build\Release"
#endif
#ifndef BuildDirX86
  #define BuildDirX86 AddBackslash(SourcePath) + "..\build-x86\Release"
#endif
#ifndef BuildDirArm64
  #define BuildDirArm64 AddBackslash(SourcePath) + "..\build-arm64\Release"
#endif
#define HaveX86 FileExists(BuildDirX86 + "\WinHookAudioMasterASIO32.dll")
#define HaveArm64 FileExists(BuildDirArm64 + "\WinHookAudioMasterASIOARM64X.dll")
#ifndef TestDriverDir
  #define TestDriverDir AddBackslash(SourcePath) + "..\build\driver"
#endif
#ifndef SignedDriverDir
  #define SignedDriverDir AddBackslash(SourcePath) + "driver-signed"
#endif
#ifndef OutputDir
  #define OutputDir AddBackslash(SourcePath) + "..\build\installer"
#endif
#define HaveSignedDriver FileExists(SignedDriverDir + "\WinHookAudio.sys")
#define HaveTestDriver FileExists(TestDriverDir + "\WinHookAudio.sys")

[Setup]
AppId=WinHookAudio
AppName=WinHookAudio
AppVersion={#AppVersion}
AppPublisher=LogicCuteGuy
AppCopyright=Copyright (c) 2026 LogicCuteGuy - MIT License
AppPublisherURL=https://github.com/LogicCuteGuy/WinHookAudio
AppSupportURL=https://github.com/LogicCuteGuy/WinHookAudio/issues
DefaultDirName={autopf}\WinHookAudio
DefaultGroupName=WinHookAudio
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=WinHookAudio-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayName=WinHookAudio
; Setup exe Properties > Details
VersionInfoCompany=LogicCuteGuy
VersionInfoCopyright=Copyright (c) 2026 LogicCuteGuy - MIT License
VersionInfoDescription=WinHookAudio Setup (made by LogicCuteGuy)
VersionInfoProductName=WinHookAudio
VersionInfoTextVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}

[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "asio"; Description: "ASIO drivers: WinHookAudio Master + Bridge 1-4"; Types: full custom; Flags: fixed
#if HaveSignedDriver || HaveTestDriver
Name: "cable"; Description: "Virtual Cable driver (8 cables as Windows sound devices)"; Types: full
#endif
#if HaveSignedDriver
Name: "cable\signed"; Description: "Signed driver (normal Windows)"; Types: full; Flags: exclusive
#endif
#if HaveTestDriver
  #if HaveSignedDriver
Name: "cable\test"; Description: "Not signed: test-signed driver, turns on Windows Test Mode (restart, Secure Boot off)"; Flags: exclusive
  #else
Name: "cable\test"; Description: "Not signed: test-signed driver, turns on Windows Test Mode (restart, Secure Boot off)"; Types: full; Flags: exclusive
  #endif
#endif

[Files]
Source: "{#BuildDir}\WinHookAudioMasterASIO64.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#BuildDir}\WinHookAudioBridgeASIO64.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
#if HaveX86
Source: "{#BuildDirX86}\WinHookAudioMasterASIO32.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#BuildDirX86}\WinHookAudioBridgeASIO32.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
#endif
#if HaveArm64
Source: "{#BuildDirArm64}\WinHookAudioMasterASIOARM64.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#BuildDirArm64}\WinHookAudioBridgeASIOARM64.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#BuildDirArm64}\WinHookAudioMasterASIOARM64X.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#BuildDirArm64}\WinHookAudioBridgeASIOARM64X.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion restartreplace uninsrestartdelete
#endif
Source: "{#BuildDir}\winhookaudio-devsetup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"
#if HaveSignedDriver
Source: "{#SignedDriverDir}\WinHookAudio.sys"; DestDir: "{app}\driver"; Components: cable\signed; Check: not IsArm64; Flags: ignoreversion
Source: "{#SignedDriverDir}\WinHookAudio.inf"; DestDir: "{app}\driver"; Components: cable\signed; Check: not IsArm64; Flags: ignoreversion
Source: "{#SignedDriverDir}\WinHookAudio.cat"; DestDir: "{app}\driver"; Components: cable\signed; Check: not IsArm64; Flags: ignoreversion
#endif
#if HaveTestDriver
Source: "{#TestDriverDir}\WinHookAudio.sys"; DestDir: "{app}\driver"; Components: cable\test; Check: not IsArm64; Flags: ignoreversion
Source: "{#TestDriverDir}\WinHookAudio.inf"; DestDir: "{app}\driver"; Components: cable\test; Check: not IsArm64; Flags: ignoreversion
Source: "{#TestDriverDir}\WinHookAudio.cat"; DestDir: "{app}\driver"; Components: cable\test; Check: not IsArm64; Flags: ignoreversion
Source: "{#TestDriverDir}\WinHookAudioTest.cer"; DestDir: "{app}\driver"; Components: cable\test; Check: not IsArm64; Flags: ignoreversion
#endif

[Registry]
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "CLSID"; ValueData: "{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Master"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 1"; ValueType: string; ValueName: "CLSID"; ValueData: "{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 1"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 1"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 2"; ValueType: string; ValueName: "CLSID"; ValueData: "{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 2"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 2"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 3"; ValueType: string; ValueName: "CLSID"; ValueData: "{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 3"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 3"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 4"; ValueType: string; ValueName: "CLSID"; ValueData: "{{B864323A-AB79-4551-B6E9-F70B61E37134}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 4"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 4"

; COM classes: the DAW loads each CLSID through InprocServer32 (same keys as DllRegisterServer)
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Master"; Flags: uninsdeletekey
#if HaveArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioMasterASIO64.dll"; Check: not IsArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioMasterASIOARM64X.dll"; Check: IsArm64
#else
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioMasterASIO64.dll"
#endif
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 1"; Flags: uninsdeletekey
#if HaveArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"; Check: not IsArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIOARM64X.dll"; Check: IsArm64
#else
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
#endif
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 2"; Flags: uninsdeletekey
#if HaveArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"; Check: not IsArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIOARM64X.dll"; Check: IsArm64
#else
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
#endif
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 3"; Flags: uninsdeletekey
#if HaveArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"; Check: not IsArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIOARM64X.dll"; Check: IsArm64
#else
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
#endif
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 4"; Flags: uninsdeletekey
#if HaveArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"; Check: not IsArm64
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIOARM64X.dll"; Check: IsArm64
#else
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
#endif
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"

; 32-bit DAWs read the 32-bit registry view (WOW6432Node): same drivers and CLSIDs, the 32-bit DLLs
#if HaveX86
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "CLSID"; ValueData: "{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Master"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Master"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioMasterASIO32.dll"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 1"; ValueType: string; ValueName: "CLSID"; ValueData: "{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 1"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 1"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 1"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO32.dll"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 2"; ValueType: string; ValueName: "CLSID"; ValueData: "{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 2"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 2"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 2"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO32.dll"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 3"; ValueType: string; ValueName: "CLSID"; ValueData: "{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 3"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 3"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 3"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO32.dll"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 4"; ValueType: string; ValueName: "CLSID"; ValueData: "{{B864323A-AB79-4551-B6E9-F70B61E37134}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 4"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 4"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 4"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO32.dll"
Root: HKLM32; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
#endif

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""WinHookAudio"" dir=in action=allow protocol=UDP localport=6980-6981"; Flags: runhidden; StatusMsg: "Configuring firewall..."

[UninstallRun]
Filename: "{app}\winhookaudio-devsetup.exe"; Parameters: "remove"; Flags: runhidden; RunOnceId: "RemoveCable"
Filename: "{sys}\certutil.exe"; Parameters: "-delstore Root ""WinHookAudio Test"""; Flags: runhidden; RunOnceId: "UntrustRoot"
Filename: "{sys}\certutil.exe"; Parameters: "-delstore TrustedPublisher ""WinHookAudio Test"""; Flags: runhidden; RunOnceId: "UntrustPublisher"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""WinHookAudio"""; Flags: runhidden; RunOnceId: "Firewall"

[Code]
const
  SetupKey = 'SOFTWARE\WinHookAudio\Setup';

var
  TestSigningWasOn: Boolean;   // before this setup ran
  RestartNeeded: Boolean;

function TestSigningOn: Boolean;
var
  Options: String;
begin
  Result := RegQueryStringValue(HKLM64, 'SYSTEM\CurrentControlSet\Control', 'SystemStartOptions', Options) and
            (Pos('TESTSIGNING', Uppercase(Options)) > 0);
end;

function SecureBootOn: Boolean;
var
  Value: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM64, 'SYSTEM\CurrentControlSet\Control\SecureBoot\State',
                               'UEFISecureBootEnabled', Value) and (Value = 1);
end;

function InitializeSetup: Boolean;
begin
  TestSigningWasOn := TestSigningOn;
  Result := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
#if HaveTestDriver
  if (CurPageID = wpSelectComponents) and WizardIsComponentSelected('cable\test') and not TestSigningWasOn and
     SecureBootOn then
    Result := SuppressibleMsgBox('Secure Boot is on, so Windows cannot turn on Test Mode and the test-signed ' +
      'Virtual Cable driver will not load.' + #13#10#13#10 +
      'Turn Secure Boot off in the PC''s UEFI settings first, or choose another option.' + #13#10#13#10 +
      'Continue anyway?', mbConfirmation, MB_YESNO, IDNO) = IDYES;
#endif
end;

// Runs a program and reports a failure; returns its exit code (-1 = could not start).
function RunStep(const What, FileName, Params: String): Integer;
begin
  WizardForm.StatusLabel.Caption := What;
  if not Exec(FileName, Params, '', SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := -1;
end;

#if HaveSignedDriver || HaveTestDriver
procedure InstallVirtualCable;
var
  Code: Integer;
  Cer, Inf: String;
begin
  Cer := ExpandConstant('{app}\driver\WinHookAudioTest.cer');
  Inf := ExpandConstant('{app}\driver\WinHookAudio.inf');
#if HaveTestDriver
  if WizardIsComponentSelected('cable\test') then begin
    RunStep('Trusting the WinHookAudio test certificate...', ExpandConstant('{sys}\certutil.exe'),
            '-f -addstore Root "' + Cer + '"');
    RunStep('Trusting the WinHookAudio test certificate...', ExpandConstant('{sys}\certutil.exe'),
            '-f -addstore TrustedPublisher "' + Cer + '"');
    if not TestSigningWasOn then begin
      Code := RunStep('Turning on Windows Test Mode...', ExpandConstant('{sys}\bcdedit.exe'), '/set testsigning on');
      if Code = 0 then begin
        RegWriteDWordValue(HKLM64, SetupKey, 'TestSigningTurnedOn', 1);
        RestartNeeded := True;
      end else
        SuppressibleMsgBox('Could not turn on Windows Test Mode (bcdedit exit ' + IntToStr(Code) + '). ' +
          'Secure Boot may be on. The Virtual Cable driver will not load until Test Mode is on.',
          mbError, MB_OK, IDOK);
    end;
  end;
#endif
  Code := RunStep('Installing the Virtual Cable driver...', ExpandConstant('{app}\winhookaudio-devsetup.exe'),
                  'install "' + Inf + '"');
  if Code = 3010 then
    RestartNeeded := True
  else if Code <> 0 then
    SuppressibleMsgBox('The Virtual Cable driver was not installed (winhookaudio-devsetup exit ' + IntToStr(Code) +
      '). The ASIO drivers are installed.' + #13#10#13#10 +
      'If Test Mode was just turned on, restart Windows, then run as administrator:' + #13#10 +
      '"' + ExpandConstant('{app}') + '\winhookaudio-devsetup.exe" install "' + Inf + '"', mbError, MB_OK, IDOK);
end;

#endif

procedure CurStepChanged(CurStep: TSetupStep);
begin
#if HaveSignedDriver || HaveTestDriver
  if (CurStep = ssPostInstall) and WizardIsComponentSelected('cable') and not IsArm64 then
    InstallVirtualCable;
#endif
end;

// Windows on ARM: the Virtual Cable driver is x64 only for now, so its choices are off (ADR 0016).
procedure CurPageChanged(CurPageID: Integer);
var
  I: Integer;
begin
  if (CurPageID = wpSelectComponents) and IsArm64 then
    for I := 1 to WizardForm.ComponentsList.Items.Count - 1 do begin  // 0 is the ASIO drivers
      WizardForm.ComponentsList.Checked[I] := False;
      WizardForm.ComponentsList.ItemEnabled[I] := False;
    end;
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;

// Uninstall: turn Test Mode off again only when this setup turned it on.
var
  TurnTestSigningOff: Boolean;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Value: Cardinal;
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then
    TurnTestSigningOff := RegQueryDWordValue(HKLM64, SetupKey, 'TestSigningTurnedOn', Value) and (Value = 1);
  if CurUninstallStep = usPostUninstall then begin
    if TurnTestSigningOff then
      Exec(ExpandConstant('{sys}\bcdedit.exe'), '/set testsigning off', '', SW_HIDE, ewWaitUntilTerminated, Code);
    RegDeleteKeyIncludingSubkeys(HKLM64, SetupKey);
    RegDeleteKeyIfEmpty(HKLM64, 'SOFTWARE\WinHookAudio');
  end;
end;
