# Code and driver signing

Two different kinds of signing, for two kinds of files:

| Files | Signed by | Why |
|---|---|---|
| `WinHookAudio-Setup-*.exe`, `WinHookAudio*ASIO64.dll`, `winhookaudio-devsetup.exe` (user mode) | **SignPath Foundation** (free for open source), from GitHub Actions | Windows SmartScreen and antivirus trust; shows a real publisher instead of "Unknown publisher" |
| `WinHookAudio.sys` + `.cat` (kernel driver) | **Microsoft** (attestation signing in Partner Center) | Windows 10 1607+ / 11 load only Microsoft-signed kernel drivers. Any other signature, SignPath's too, still needs Test Mode |

Until the driver is Microsoft-signed, releases offer only the test-signed driver
([installing.md](installing.md)).

## SignPath (user-mode files)

### SignPath Foundation's conditions (signpath.org/terms)

- The repository must be **public**, with an OSI-approved license: MIT ([LICENSE](../LICENSE)).
- No proprietary component and no commercially dual-licensed component. Release builds do **not**
  use the Steinberg ASIO SDK (dual-licensed): CI compiles against `common/WHAAsio.h`
  ([THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)). Do not put `third_party/asio/` into a
  signed build.
- The binaries must be built by GitHub Actions from this repository (SignPath checks the build).

### Set up the GitHub side (after SignPath accepts the project)

1. In SignPath: add the GitHub trusted build system, link this repository, and create two
   artifact configurations:
   - slug `binaries`: the zip of the DLLs + devsetup exe
     ```xml
     <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
       <zip-file>
         <pe-file-set>
           <include path="*.dll" max-matches="unbounded" />
           <include path="*.exe" max-matches="unbounded" />
           <for-each><authenticode-sign /></for-each>
         </pe-file-set>
       </zip-file>
     </artifact-configuration>
     ```
   - slug `installer`: the zip holding `WinHookAudio-Setup-*.exe`
     ```xml
     <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
       <zip-file>
         <pe-file path="WinHookAudio-Setup-*.exe"><authenticode-sign /></pe-file>
       </zip-file>
     </artifact-configuration>
     ```
2. In GitHub > Settings > Secrets and variables > Actions:
   - variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_SIGNING_POLICY_SLUG`
     (for example `release-signing`)
   - secret `SIGNPATH_API_TOKEN`
3. Push. [.github/workflows/build.yml](../.github/workflows/build.yml) signs the binaries before it
   builds the installer, then signs the installer. Without `SIGNPATH_ORGANIZATION_ID` those steps
   are skipped and the build is unsigned.

Note: the uninstaller Inno Setup writes at install time (`unins000.exe`) is not signed this way.

## Microsoft-signed driver (normal Windows, no Test Mode)

Attestation signing, for Windows 10/11 client PCs:

1. Get an **EV code-signing certificate** (a company or registered sole trader; SignPath
   Foundation's certificate is not EV and cannot register).
2. Register in the Windows Hardware Dev Center (Partner Center) with it.
3. Build the driver (`driver\build.cmd`), put `WinHookAudio.sys`, `.inf`, `.pdb` in a CAB, sign
   the CAB with the EV certificate, submit it for **attestation signing**.
4. Microsoft returns the package signed. Put `WinHookAudio.sys`, `WinHookAudio.inf`,
   `WinHookAudio.cat` in [installer/driver-signed/](../installer/driver-signed/) and commit them.
5. `installer\build-installer.ps1` (local or CI) then adds the **Signed driver** choice and
   selects it by default. The test-signed choice stays for developers.

Rebuild and re-submit whenever `driver/` changes: the Microsoft-signed files must match the
driver version you release.
