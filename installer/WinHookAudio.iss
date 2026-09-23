; WinHookAudio Installer — Inno Setup 6.x
; 5 CLSIDs + WinHookAudio.sys + WinHookAudio.inf/.cat + Firewall 6980-6981

#define MyAppName "WinHookAudio"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "WinHookAudio"

[Setup]
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={pf}\WinHookAudio
DefaultGroupName=WinHookAudio
OutputBaseFilename=WinHookAudio-Setup
Compression=lzma
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64

[Tasks]
Name: "testsigning"; Description: "Enable Windows test-signing (only for a test-signed Virtual Cable driver; needs reboot)"; Flags: unchecked

[Files]
Source: "..\build\Release\WinHookAudioMasterASIO64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\WinHookAudioBridgeASIO64.dll"; DestDir: "{app}"; Flags: ignoreversion
; Virtual Cable kernel driver (WDK): optional until WinHookAudio.sys is built and signed
Source: "WinHookAudio.sys"; DestDir: "{app}\driver"; Flags: ignoreversion skipifsourcedoesntexist
Source: "WinHookAudio.inf"; DestDir: "{app}\driver"; Flags: ignoreversion skipifsourcedoesntexist
Source: "WinHookAudio.cat"; DestDir: "{app}\driver"; Flags: ignoreversion skipifsourcedoesntexist

[Registry]
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "CLSID"; ValueData: "{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Master (512)"
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
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioMasterASIO64.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{CB739B1A-D8A2-409D-A8B7-8CFA4B699C76}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 1"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{A7C1B6FB-9A84-4E9D-A305-EA24944014BD}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 2"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{939344AC-BE3F-44EB-A1C3-90691D8F1C9E}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 3"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{3CDDA02A-0A66-45C9-B612-1FD2169EB8E8}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}"; ValueType: string; ValueName: ""; ValueData: "WinHookAudio Bridge 4"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\WinHookAudioBridgeASIO64.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{B864323A-AB79-4551-B6E9-F70B61E37134}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"

[Run]
Filename: "pnputil"; Parameters: "/add-driver ""{app}\driver\WinHookAudio.inf"" /install"; Flags: runhidden; StatusMsg: "Installing WinHookAudio driver..."; Check: VirtualCableIncluded
Filename: "bcdedit"; Parameters: "/set testsigning on"; Flags: runhidden; StatusMsg: "Enabling testsigning..."; Tasks: testsigning
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""WinHookAudio"" dir=in action=allow protocol=UDP localport=6980-6981"; Flags: runhidden; StatusMsg: "Configuring firewall..."

[UninstallRun]
Filename: "pnputil"; Parameters: "/delete-driver WinHookAudio.inf /uninstall /force"; Flags: runhidden; Check: VirtualCableIncluded
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""WinHookAudio"""; Flags: runhidden

[Code]
function VirtualCableIncluded: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\driver\WinHookAudio.sys'));
end;
