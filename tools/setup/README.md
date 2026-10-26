# Dev PC setup (and redoing it after a Windows reinstall)

The project folder `D:\Remote streaming` holds the repository (`src`), the toolchain, the EWDK, MSYS2, git, gh, caches
and the test VM files. As long as D: is not formatted, a clean Windows install only loses what lived on C:. To redo it:

| Step | Who | How |
|---|---|---|
| 1. Map T: to the project folder, put git and gh on PATH | anyone | `. 'D:\Remote streaming\src\tools\setup\env.ps1'` (each logon) |
| 2. Python 3.14+ (used by `tools/repo/*.py`, `tools/setup/msys2-fix-hardlinks.py`) | anyone | `winget install --id Python.Python.3.14 --scope user` |
| 3. GitHub login | **user** | `gh auth login --web --git-protocol https --scopes repo,workflow` (browser) |
| 4. git config | anyone | `git config --global --add safe.directory "D:/Remote streaming/*"` and `... "T:/*"`, `gc.auto 0`, `maintenance.auto false` (exFAT, see ADR 0004), then `gh auth setup-git`. git itself is portable MinGit in `toolchain\git`. |
| 5. New dev signing certificate | anyone | The old key was non-exportable and is gone with the reinstall. `New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Tessera Dev Driver Signing (local only)' -KeyExportPolicy NonExportable -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3)`. The scripts find it by subject, so no thumbprint needs to be copied anywhere. |
| 6. Hyper-V, Hyper-V Administrators, trust the certificate | **user, elevated, once** | From the developer's own account (as admin): `powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\setup\admin-once.ps1"`, then reboot. Undo: `admin-undo.ps1`. |
| 7. Test VM | anyone (after 6) | The old VM can't be re-registered: its vTPM is sealed to the previous Windows installation's guardian keys. Delete `D:\Remote streaming\vm\tessera-test`, then recreate it with `tools/vm/New-TestVm.ps1` (~30 min unattended install). |

Nothing else is needed. MSYS2, CMake, Ninja, git, gh, the EWDK and the caches are portable and stay on D:.
