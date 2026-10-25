# Dev PC setup (and redoing it after a Windows reinstall)

The project folder `D:\Remote streaming` holds the repository (`src`), the toolchain, the EWDK, MSYS2, caches and the
test VM files. As long as D: is not formatted, a clean Windows install only loses what lived on C:. To redo it:

| Step | Who | How |
|---|---|---|
| 1. Map T: to the project folder | anyone | `. 'D:\Remote streaming\src\tools\setup\env.ps1'` (each logon) |
| 2. Python 3.14+ (used by `tools/repo/*.py`, `tools/setup/msys2-fix-hardlinks.py`) | user | Install from python.org (or `winget install Python.Python.3.14`) |
| 3. GitHub login | **user** | `T:\toolchain\gh\bin\gh.exe auth login --web --scopes repo,workflow` |
| 4. git on exFAT | anyone | `git config --global --add safe.directory "D:/Remote streaming/*"` and `... "T:/*"` |
| 5. New dev signing certificate | anyone | The old key was non-exportable and is gone with the reinstall. Create one with `New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Tessera Dev Driver Signing (local only)' -KeyExportPolicy NonExportable -CertStoreLocation Cert:\CurrentUser\My`, export the `.cer`, and pass the new thumbprint to the next step and to `tools/signing/Sign-DriverPackage.ps1 -Thumbprint`. |
| 6. Hyper-V, Hyper-V Administrators, trust the certificate | **user, elevated, once** | `tools/setup/admin-once.ps1 -User <PC>\<user> -CertPath <new .cer> -ExpectedThumbprint <new thumbprint>`, then reboot |
| 7. Test VM | anyone (after 6) | The old VM can't be re-registered: its vTPM is sealed to this Windows installation's guardian keys. Recreate it with `tools/vm/New-TestVm.ps1` (~30 min unattended install). The old `D:\Remote streaming\vm\tessera-test` folder can be deleted first. |

Nothing else is needed. MSYS2, CMake, Ninja, gh, the EWDK and the caches are portable and stay on D:.
