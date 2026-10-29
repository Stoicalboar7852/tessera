/**
 * @file tools/vdd-install/src/vdd_install.cpp
 * @brief vdd-install.exe: installs, removes and queries the root-enumerated Tessera virtual display device.
 *
 * Only the installer (with admin consent) installs drivers (brief §4.4); this tool is what it runs.
 *   vdd-install install <path\TesseraVdd.inf>   create Root\TesseraVdd if needed, install/update the driver
 *   vdd-install remove [--purge]                remove the device(s); --purge also deletes the driver package
 *   vdd-install status                          list devices, their state and problem code
 *   vdd-install query                           open the control interface and print the driver version
 *   vdd-install forget-monitors [--settings] [--dry-run]
 *                                               delete the monitor devices Windows keeps for Tessera monitors that
 *                                               are gone (one per monitor ID ever used); --settings also deletes
 *                                               Windows' saved display settings for Tessera monitors (only while
 *                                               none is connected)
 * `--hwid ID` (before or after the command) works on another root-enumerated display device instead, e.g. the
 * third-party VDDs that tools/vdd-stress compares against (Root\SudoMaker\SudoVDA, Root\Parsec\VDA, Root\MttVDD).
 * Exit codes: 0 ok, 1 failure, 2 usage, 3 reboot required.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

// platform includes
#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#include <initguid.h>
#include "tessera_vdd_ioctl.h"

// local includes
#include "tessera/win32.h"

using namespace tessera;

namespace {

  std::wstring hardware_id = L"Root\\TesseraVdd";  ///< Replaced by --hwid.

  /// A zeroed SetupAPI struct with its cbSize set.
  template<class T>
  T sized() {
    T v {};
    v.cbSize = sizeof(T);
    return v;
  }

  int fail(const error_t &e) {
    std::fprintf(stderr, "ERROR: %s\n", e.to_string().c_str());
    return 1;
  }

  /// RAII for HDEVINFO.
  struct devinfo_t {
    HDEVINFO h = INVALID_HANDLE_VALUE;

    ~devinfo_t() {
      if (h != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(h);
      }
    }
  };

  /// Calls fn(set, data) for each present or non-present device whose hardware IDs include ours.
  /// All classes are searched: a device whose driver install failed no longer reports the Display class.
  template<class Fn>
  result_t<int> for_each_device(Fn &&fn) {
    devinfo_t set;
    set.h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set.h == INVALID_HANDLE_VALUE) {
      return win32::last_error("SetupDiGetClassDevsW");
    }
    int count = 0;
    auto data = sized<SP_DEVINFO_DATA>();
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &data); ++i) {
      wchar_t ids[512] = {};
      if (!SetupDiGetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids), sizeof(ids) - sizeof(wchar_t), nullptr)) {
        continue;
      }
      for (const wchar_t *p = ids; *p != L'\0'; p += wcslen(p) + 1) {
        if (_wcsicmp(p, hardware_id.c_str()) == 0) {
          ++count;
          fn(set.h, data);
          break;
        }
      }
    }
    return count;
  }

  std::string instance_id(HDEVINFO set, SP_DEVINFO_DATA &data) {
    wchar_t buf[MAX_DEVICE_ID_LEN] = {};
    SetupDiGetDeviceInstanceIdW(set, &data, buf, MAX_DEVICE_ID_LEN, nullptr);
    return win32::to_utf8(buf);
  }

  int cmd_status() {
    auto n = for_each_device([](HDEVINFO set, SP_DEVINFO_DATA &data) {
      ULONG status = 0;
      ULONG problem = 0;
      const CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0);
      if (cr == CR_NO_SUCH_DEVNODE) {
        std::printf("%s  not present\n", instance_id(set, data).c_str());
      } else if (cr != CR_SUCCESS) {
        std::printf("%s  CM_Get_DevNode_Status failed (CONFIGRET %lu)\n", instance_id(set, data).c_str(), cr);
      } else {
        const bool started = (status & DN_STARTED) != 0;
        std::printf("%s  %s%s\n", instance_id(set, data).c_str(), started ? "started" : "not started", (status & DN_HAS_PROBLEM) ? (" problem code " + std::to_string(problem)).c_str() : "");
      }
    });
    if (!n) {
      return fail(n.error());
    }
    if (n.value() == 0) {
      std::printf("no device with hardware ID %s\n", win32::to_utf8(hardware_id).c_str());
    }
    return 0;
  }

  int cmd_install(const std::wstring &inf) {
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(inf.c_str(), MAX_PATH, full, nullptr) == 0) {
      return fail(win32::last_error("GetFullPathNameW", inf));
    }
    auto existing = for_each_device([](HDEVINFO, SP_DEVINFO_DATA &) {});
    if (!existing) {
      return fail(existing.error());
    }
    if (existing.value() == 0) {
      // Create the root-enumerated device node (what devcon "install" does).
      devinfo_t set;
      set.h = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
      if (set.h == INVALID_HANDLE_VALUE) {
        return fail(win32::last_error("SetupDiCreateDeviceInfoList"));
      }
      auto data = sized<SP_DEVINFO_DATA>();
      if (!SetupDiCreateDeviceInfoW(set.h, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DICD_GENERATE_ID, &data)) {
        return fail(win32::last_error("SetupDiCreateDeviceInfoW"));
      }
      const std::wstring ids = hardware_id + L'\0';  // REG_MULTI_SZ: the string's own NUL plus this one
      if (!SetupDiSetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, reinterpret_cast<const BYTE *>(ids.c_str()), static_cast<DWORD>((ids.size() + 1) * sizeof(wchar_t)))) {
        return fail(win32::last_error("SetupDiSetDeviceRegistryPropertyW(HARDWAREID)"));
      }
      if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.h, &data)) {
        return fail(win32::last_error("SetupDiCallClassInstaller(DIF_REGISTERDEVICE)"));
      }
      std::printf("created device %s\n", instance_id(set.h, data).c_str());
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardware_id.c_str(), full, INSTALLFLAG_FORCE, &reboot)) {
      return fail(win32::last_error("UpdateDriverForPlugAndPlayDevicesW", full));
    }
    std::printf("driver installed from %s%s\n", win32::to_utf8(full).c_str(), reboot ? " (reboot required)" : "");
    return reboot ? 3 : 0;
  }

  /// "TSR" plus a 4-digit hex product code: a monitor ID from a Tessera EDID (display_modes.h).
  bool is_tessera_monitor_id(const std::wstring &s) {
    if (s.size() < 7 || _wcsnicmp(s.c_str(), L"TSR", 3) != 0) {
      return false;
    }
    return std::all_of(s.begin() + 3, s.begin() + 7, [](wchar_t c) {
      return std::iswxdigit(c) != 0;
    });
  }

  /**
   * @brief Windows' display database keys a saved setting by monitor ID, or by a monitor set ("ID+ID+...^hash").
   *        True if any monitor in the key is a Tessera monitor.
   */
  bool mentions_tessera_monitor(const std::wstring &key) {
    const std::wstring ids = key.substr(0, key.find(L'^'));
    for (std::size_t start = 0; start <= ids.size();) {
      const std::size_t end = std::min(ids.find_first_of(L"+*", start), ids.size());
      if (is_tessera_monitor_id(ids.substr(start, end - start))) {
        return true;
      }
      start = end + 1;
    }
    return false;
  }

  /// Deletes the subkeys of HKLM\...\GraphicsDrivers\<name> that mention a Tessera monitor. Returns the count.
  int forget_settings(const wchar_t *name, bool dry_run, int &failures) {
    const std::wstring path = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\") + name;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | (dry_run ? 0 : DELETE | KEY_SET_VALUE), &key) != ERROR_SUCCESS) {
      return 0;  // not every Windows version has every key
    }
    std::vector<std::wstring> doomed;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
      DWORD len = static_cast<DWORD>(std::size(sub));
      if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        break;
      }
      if (mentions_tessera_monitor(sub)) {
        doomed.emplace_back(sub);
      }
    }
    int deleted = 0;
    for (const auto &d : doomed) {
      if (dry_run) {
        ++deleted;
      } else if (const LSTATUS rc = RegDeleteTreeW(key, d.c_str()); rc == ERROR_SUCCESS) {
        ++deleted;
      } else {
        ++failures;
        if (failures <= 5) {
          std::fprintf(stderr, "ERROR: %s\n", win32::win32_error(static_cast<DWORD>(rc), "RegDeleteTreeW", win32::to_utf8(path + L"\\" + d)).to_string().c_str());
        }
      }
    }
    RegCloseKey(key);
    std::printf("%s %d saved setting(s) in GraphicsDrivers\\%s\n", dry_run ? "would delete" : "deleted", deleted, win32::to_utf8(name).c_str());
    return deleted;
  }

  /**
   * @brief Every monitor ID a Tessera monitor ever had leaves a monitor device behind (DISPLAY\TSRxxxx\...),
   *        and Windows deletes them one by one (about 150 ms each) when the adapter is removed. Deleting the ones
   *        that are gone first keeps the adapter removal short and shows progress.
   */
  int cmd_forget(bool settings, bool dry_run) {
    devinfo_t set;
    set.h = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, L"DISPLAY", nullptr, 0);  // present and non-present
    if (set.h == INVALID_HANDLE_VALUE) {
      return fail(win32::last_error("SetupDiGetClassDevsW(Monitor)"));
    }
    int present = 0;
    int gone = 0;
    int removed = 0;
    int failures = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto data = sized<SP_DEVINFO_DATA>();
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &data); ++i) {
      const std::string id = instance_id(set.h, data);
      if (id.size() < 15 || _strnicmp(id.c_str(), "DISPLAY\\", 8) != 0 || !is_tessera_monitor_id(win32::to_wide(id.substr(8, 7)))) {
        continue;
      }
      ULONG status = 0;
      ULONG problem = 0;
      if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) != CR_NO_SUCH_DEVNODE) {
        ++present;
        continue;
      }
      ++gone;
      if (dry_run) {
        continue;
      }
      BOOL reboot = FALSE;
      if (DiUninstallDevice(nullptr, set.h, &data, 0, &reboot)) {
        if (++removed % 250 == 0) {
          const auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
          std::printf("  %d removed (%lld s)\n", removed, static_cast<long long>(s));
          std::fflush(stdout);
        }
      } else if (++failures <= 5) {
        std::fprintf(stderr, "ERROR: %s\n", win32::last_error("DiUninstallDevice", id).to_string().c_str());
      }
    }
    std::printf("Tessera monitor devices: %d connected, %d gone, %s %d\n", present, gone, dry_run ? "would remove" : "removed", dry_run ? gone : removed);
    if (settings) {
      if (present != 0) {
        std::fputs("ERROR: saved display settings are kept while a Tessera monitor is connected\n", stderr);
        return 1;
      }
      for (const wchar_t *name : {L"Configuration", L"Connectivity", L"ScaleFactors", L"MonitorDataStore"}) {
        forget_settings(name, dry_run, failures);
      }
    }
    return failures ? 1 : 0;
  }

  int cmd_remove(bool purge) {
    if (_wcsicmp(hardware_id.c_str(), L"Root\\TesseraVdd") == 0) {
      cmd_forget(false, false);  // best effort: the adapter removal deletes what is left
    }
    std::vector<std::wstring> oem_infs;
    bool reboot_needed = false;
    int failures = 0;
    auto n = for_each_device([&](HDEVINFO set, SP_DEVINFO_DATA &data) {
      if (purge) {
        // Remember the driver package before the device goes away.
        auto drv = sized<SP_DRVINFO_DATA_W>();
        auto params = sized<SP_DEVINSTALL_PARAMS_W>();
        if (SetupDiGetDeviceInstallParamsW(set, &data, &params)) {
          params.FlagsEx |= DI_FLAGSEX_INSTALLEDDRIVER;
          SetupDiSetDeviceInstallParamsW(set, &data, &params);
          if (SetupDiBuildDriverInfoList(set, &data, SPDIT_CLASSDRIVER) && SetupDiEnumDriverInfoW(set, &data, SPDIT_CLASSDRIVER, 0, &drv)) {
            SP_DRVINFO_DETAIL_DATA_W detail {};
            detail.cbSize = sizeof(detail);
            if (SetupDiGetDriverInfoDetailW(set, &data, &drv, &detail, sizeof(detail), nullptr) || GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
              std::wstring path = detail.InfFileName;
              std::wstring oem = path.substr(path.find_last_of(L'\\') + 1);
              if (std::find(oem_infs.begin(), oem_infs.end(), oem) == oem_infs.end()) {
                oem_infs.push_back(std::move(oem));  // several devices can share one driver package
              }
            }
          }
        }
      }
      const std::string id = instance_id(set, data);
      BOOL reboot = FALSE;
      if (DiUninstallDevice(nullptr, set, &data, 0, &reboot)) {
        std::printf("removed %s\n", id.c_str());
        reboot_needed |= reboot != FALSE;
      } else {
        std::fprintf(stderr, "ERROR: %s\n", win32::last_error("DiUninstallDevice", id).to_string().c_str());
        ++failures;
      }
    });
    if (!n) {
      return fail(n.error());
    }
    for (const auto &oem : oem_infs) {
      if (SetupUninstallOEMInfW(oem.c_str(), SUOI_FORCEDELETE, nullptr)) {
        std::printf("deleted driver package %s\n", win32::to_utf8(oem).c_str());
      } else {
        std::fprintf(stderr, "ERROR: %s\n", win32::last_error("SetupUninstallOEMInfW", oem).to_string().c_str());
        ++failures;
      }
    }
    if (n.value() == 0) {
      std::printf("no device with hardware ID %s\n", win32::to_utf8(hardware_id).c_str());
    }
    return failures ? 1 : (reboot_needed ? 3 : 0);
  }

  int cmd_query() {
    devinfo_t set;
    set.h = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_TESSERA_VDD, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set.h == INVALID_HANDLE_VALUE) {
      return fail(win32::last_error("SetupDiGetClassDevsW(interface)"));
    }
    auto iface = sized<SP_DEVICE_INTERFACE_DATA>();
    if (!SetupDiEnumDeviceInterfaces(set.h, nullptr, &GUID_DEVINTERFACE_TESSERA_VDD, 0, &iface)) {
      return fail(make_error(tessera_code_e::not_found, "vdd-install query", "no Tessera VDD control interface (driver not running?)"));
    }
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set.h, &iface, nullptr, 0, &needed, nullptr);
    std::vector<BYTE> buf(needed);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set.h, &iface, detail, needed, nullptr, nullptr)) {
      return fail(win32::last_error("SetupDiGetDeviceInterfaceDetailW"));
    }
    win32::unique_handle_t dev(CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!dev) {
      return fail(win32::last_error("CreateFileW(VDD interface)", "needs SYSTEM or Administrators"));
    }
    tessera_vdd_version_t v {};
    DWORD got = 0;
    if (!DeviceIoControl(dev.get(), IOCTL_TESSERA_VDD_GET_VERSION, nullptr, 0, &v, sizeof(v), &got, nullptr)) {
      return fail(win32::last_error("DeviceIoControl(GET_VERSION)"));
    }
    if (got < sizeof(v) || v.size != sizeof(v)) {
      return fail(make_error(tessera_code_e::invalid_data, "vdd-install query", "unexpected response size " + std::to_string(got)));
    }
    std::printf("Tessera VDD %u.%u.%u, interface v%u, monitors %u/%u\n", v.driver_major, v.driver_minor, v.driver_patch, v.interface_version, v.active_monitors, v.max_monitors);
    return 0;
  }

  void usage() {
    std::puts("Usage: vdd-install [--hwid ID] install <file.inf> | remove [--purge] | status | query\n"
              "       vdd-install forget-monitors [--settings] [--dry-run]");
  }

}  // namespace

int wmain(int argc, wchar_t **argv) {
  std::vector<std::wstring> args;
  for (int i = 1; i < argc; ++i) {
    if (std::wstring(argv[i]) == L"--hwid" && i + 1 < argc) {
      hardware_id = argv[++i];
    } else {
      args.emplace_back(argv[i]);
    }
  }
  if (args.empty() || hardware_id.empty() || hardware_id.size() > 200) {
    usage();
    return 2;
  }
  const std::wstring &cmd = args[0];
  if (cmd == L"install" && args.size() == 2) {
    return cmd_install(args[1]);
  }
  if (cmd == L"remove" && (args.size() == 1 || (args.size() == 2 && args[1] == L"--purge"))) {
    return cmd_remove(args.size() == 2);
  }
  if (cmd == L"status" && args.size() == 1) {
    return cmd_status();
  }
  if (cmd == L"query" && args.size() == 1) {
    return cmd_query();
  }
  if (cmd == L"forget-monitors") {
    bool settings = false;
    bool dry_run = false;
    bool valid = true;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == L"--settings") {
        settings = true;
      } else if (args[i] == L"--dry-run") {
        dry_run = true;
      } else {
        valid = false;
      }
    }
    if (valid) {
      return cmd_forget(settings, dry_run);
    }
  }
  usage();
  return 2;
}
