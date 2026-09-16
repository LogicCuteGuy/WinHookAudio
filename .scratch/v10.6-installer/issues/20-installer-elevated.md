# 20: Installer elevated + EV Cert

**What to build:** `iscc` + `pnputil` + `netsh` + `EV Cert` + `Microsoft Attestation` + `bcdedit` `testsigning` + `ADMIN=True` + `Bitwig` runtime gate.

**Blocked by:** 19-installer-reg-iss.

**Status:** ready-for-agent

- [ ] `iscc` + `pnputil` + `netsh` + `EV Cert` + `Microsoft Attestation` + `bcdedit` `testsigning` + `ADMIN=True` + `Bitwig` runtime gate — `iscc` builds `WinHookAudio-Setup.exe`, `pnputil /add-driver WinHookAudio.inf /install` installs `WinHookAudio.sys` `8× Stereo` `WDK` `PortCls` `WaveRT` `Root\WinHookAudio`, `netsh advfirewall firewall add rule name="WinHookAudio" dir=in action=allow protocol=UDP localport=6980-6981` allows `WHAA` `UDP 6980`/`6981`, `EV Cert` + `Attestation` for `Win11` normal mode, `bcdedit /set testsigning on` for offline `test` cert, `Bitwig` `Preferences → Audio → ASIO → WinHookAudio Master` shows `512` names, `stream_verified:true` only after measured tone
