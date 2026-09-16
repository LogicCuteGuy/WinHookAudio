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

[Files]
Source: "..\build\Release\WinHookAudioMasterASIO64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\WinHookAudioBridgeASIO64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "WinHookAudio.sys"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "WinHookAudio.inf"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "WinHookAudio.cat"; DestDir: "{app}\driver"; Flags: ignoreversion

[Registry]
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "CLSID"; ValueData: "{12345678-1234-1234-1234-56789abcdef0}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Master"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Master (512)"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 1"; ValueType: string; ValueName: "CLSID"; ValueData: "{12345678-1234-1234-1234-56789abcdef1}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 1"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 1"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 2"; ValueType: string; ValueName: "CLSID"; ValueData: "{12345678-1234-1234-1234-56789abcdef2}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 2"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 2"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 3"; ValueType: string; ValueName: "CLSID"; ValueData: "{12345678-1234-1234-1234-56789abcdef3}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 3"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 3"
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 4"; ValueType: string; ValueName: "CLSID"; ValueData: "{12345678-1234-1234-1234-56789abcdef4}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ASIO\WinHookAudio Bridge 4"; ValueType: string; ValueName: "Description"; ValueData: "WinHookAudio Bridge 4"

[Run]
Filename: "pnputil"; Parameters: "/add-driver ""{app}\driver\WinHookAudio.inf"" /install"; Flags: runhidden; StatusMsg: "Installing WinHookAudio driver..."
Filename: "bcdedit"; Parameters: "/set testsigning on"; Flags: runhidden; StatusMsg: "Enabling testsigning..."
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""WinHookAudio"" dir=in action=allow protocol=UDP localport=6980-6981"; Flags: runhidden; StatusMsg: "Configuring firewall..."

[UninstallRun]
Filename: "pnputil"; Parameters: "/delete-driver WinHookAudio.inf /uninstall /force"; Flags: runhidden
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""WinHookAudio"""; Flags: runhidden
