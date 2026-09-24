# Microsoft-signed Virtual Cable driver

Put the package Microsoft returns from attestation signing here:

```
installer/driver-signed/WinHookAudio.sys
installer/driver-signed/WinHookAudio.inf
installer/driver-signed/WinHookAudio.cat
```

When `WinHookAudio.sys` is here, `installer\build-installer.ps1` adds the **Signed driver (normal
Windows)** choice to the setup and selects it by default. Empty = setup offers only the test-signed
driver. How to get the package: [docs/signing.md](../../docs/signing.md).
