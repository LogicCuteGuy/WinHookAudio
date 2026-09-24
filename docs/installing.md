# Installing WinHookAudio

`WinHookAudio-Setup-<version>.exe` installs (admin rights needed):

| Part | Where |
|---|---|
| ASIO drivers Master + Bridge 1-4 | `C:\Program Files\WinHookAudio\WinHookAudio*ASIO64.dll`, registered under `HKLM\SOFTWARE\ASIO` (5 entries) |
| Virtual Cable driver (optional) | `C:\Program Files\WinHookAudio\driver\`, device `Root\WinHookAudio` (8 cables in Windows Sound settings) |
| Firewall rule "WinHookAudio" | UDP 6980-6981 in, for network streams |
| `winhookaudio-devsetup.exe` | creates and removes the `Root\WinHookAudio` device |

Your settings live in `%ProgramData%\WinHookAudio` (`routes.yml`, `settings.yml`). Setup and
uninstall do not touch them.

## Virtual Cable: signed or not signed

Windows 10 (1607 and later) and Windows 11 load a kernel driver only when **Microsoft** signed it.
A normal code-signing certificate, including SignPath's, is not enough for a kernel driver
([docs/signing.md](signing.md)). So the installer offers:

| Components page | What setup does | You need |
|---|---|---|
| **Signed driver (normal Windows)** | installs the Microsoft-signed package | nothing; only listed when the release has one |
| **Not signed: test-signed driver** | trusts the "WinHookAudio Test" certificate (Root + TrustedPublisher), runs `bcdedit /set testsigning on`, installs the test-signed package | Secure Boot off, then a restart |
| Virtual Cable unticked | no driver | nothing |

### Test Mode steps

1. Turn **Secure Boot off** in the PC's UEFI (BIOS) settings. With Secure Boot on, Windows refuses
   Test Mode; setup warns you before it starts.
2. Run setup and choose **Not signed: test-signed driver**.
3. Restart when setup asks. The desktop then shows "Test Mode" in the corner.
4. Windows Sound settings should list the 8 WinHookAudio cables. The Control Panel's ABOUT tab shows
   `WinHookAudio.sys: running`.

Test Mode lowers Windows' driver protection: any test-signed driver can load. Use it on a PC you
trust, and prefer the signed driver when a release has it.

## Silent install

```bat
WinHookAudio-Setup-1.2.3.exe /VERYSILENT /COMPONENTS="asio,cable\test"
WinHookAudio-Setup-1.2.3.exe /VERYSILENT /COMPONENTS="asio"
```

## Uninstall

Windows Settings > Apps > WinHookAudio. It removes the DLLs and ASIO entries, the
`Root\WinHookAudio` device and its driver packages, the test certificate and the firewall rule. It
runs `bcdedit /set testsigning off` only when this setup turned Test Mode on.

## Check the Virtual Cable driver

```bat
"C:\Program Files\WinHookAudio\winhookaudio-devsetup.exe" status
```

prints the number of `Root\WinHookAudio` devices, the driver packages in the driver store and
whether `WinHookAudio.sys` is running. It changes nothing.

## Developers

`driver\install.ps1` installs the driver from `build\driver` on a test PC without the installer
(needs `devcon` from the WDK). `installer\build-installer.ps1` builds the setup; see the README.
