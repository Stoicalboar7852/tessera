/**
 * @file tools/vdd-install/src/vdd_install.cpp
 * @brief vdd-install.exe: installs, removes and queries the root-enumerated Tessera virtual display device.
 *
 * Only the installer (with admin consent) installs drivers (brief §4.4); this tool is what it runs.
 *   vdd-install install <path\TesseraVdd.inf>   create Root\TesseraVdd if needed, install/update the driver
 *   vdd-install remove [--purge]                remove the device(s); --purge also deletes the driver package
 *   vdd-install status                          list devices, their state and problem code
 *   vdd-install query                           open the control interface and print the driver version
 * Exit codes: 0 ok, 1 failure, 2 usage, 3 reboot required.
 */
// standard includes
#include <cstdio>
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

  constexpr wchar_t hardware_id[] = L"Root\\TesseraVdd";

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
  template<class Fn>
  result_t<int> for_each_device(Fn &&fn) {
    devinfo_t set;
    set.h = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set.h == INVALID_HANDLE_VALUE) {
      return win32::last_error("SetupDiGetClassDevsW");
    }
    int count = 0;
    SP_DEVINFO_DATA data {sizeof(SP_DEVINFO_DATA)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &data); ++i) {
      wchar_t ids[512] = {};
      if (!SetupDiGetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids), sizeof(ids) - sizeof(wchar_t), nullptr)) {
        continue;
      }
      for (const wchar_t *p = ids; *p != L'\0'; p += wcslen(p) + 1) {
        if (_wcsicmp(p, hardware_id) == 0) {
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
      std::puts("no Tessera virtual display device");
    }
    return 0;
  }

  int cmd_install(const std::wstring &inf) {
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(inf.c_str(), MAX_PATH, full, nullptr) == 0) {
      return fail(win32::last_error("GetFullPathNameW", win32::to_utf8(inf)));
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
      SP_DEVINFO_DATA data {sizeof(SP_DEVINFO_DATA)};
      if (!SetupDiCreateDeviceInfoW(set.h, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DICD_GENERATE_ID, &data)) {
        return fail(win32::last_error("SetupDiCreateDeviceInfoW"));
      }
      const wchar_t ids[] = L"Root\\TesseraVdd\0";  // REG_MULTI_SZ, double NUL-terminated
      if (!SetupDiSetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, reinterpret_cast<const BYTE *>(ids), sizeof(ids))) {
        return fail(win32::last_error("SetupDiSetDeviceRegistryPropertyW(HARDWAREID)"));
      }
      if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.h, &data)) {
        return fail(win32::last_error("SetupDiCallClassInstaller(DIF_REGISTERDEVICE)"));
      }
      std::printf("created device %s\n", instance_id(set.h, data).c_str());
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardware_id, full, INSTALLFLAG_FORCE, &reboot)) {
      return fail(win32::last_error("UpdateDriverForPlugAndPlayDevicesW", win32::to_utf8(full)));
    }
    std::printf("driver installed from %s%s\n", win32::to_utf8(full).c_str(), reboot ? " (reboot required)" : "");
    return reboot ? 3 : 0;
  }

  int cmd_remove(bool purge) {
    std::vector<std::wstring> oem_infs;
    bool reboot_needed = false;
    int failures = 0;
    auto n = for_each_device([&](HDEVINFO set, SP_DEVINFO_DATA &data) {
      if (purge) {
        // Remember the driver package before the device goes away.
        SP_DRVINFO_DATA_W drv {sizeof(SP_DRVINFO_DATA_W)};
        SP_DEVINSTALL_PARAMS_W params {sizeof(SP_DEVINSTALL_PARAMS_W)};
        if (SetupDiGetDeviceInstallParamsW(set, &data, &params)) {
          params.FlagsEx |= DI_FLAGSEX_INSTALLEDDRIVER;
          SetupDiSetDeviceInstallParamsW(set, &data, &params);
          if (SetupDiBuildDriverInfoList(set, &data, SPDIT_CLASSDRIVER) && SetupDiEnumDriverInfoW(set, &data, SPDIT_CLASSDRIVER, 0, &drv)) {
            SP_DRVINFO_DETAIL_DATA_W detail {};
            detail.cbSize = sizeof(detail);
            if (SetupDiGetDriverInfoDetailW(set, &data, &drv, &detail, sizeof(detail), nullptr) || GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
              std::wstring path = detail.InfFileName;
              oem_infs.push_back(path.substr(path.find_last_of(L'\\') + 1));
            }
          }
        }
      }
      BOOL reboot = FALSE;
      if (DiUninstallDevice(nullptr, set, &data, 0, &reboot)) {
        std::printf("removed %s\n", instance_id(set, data).c_str());
        reboot_needed |= reboot != FALSE;
      } else {
        std::fprintf(stderr, "ERROR: %s\n", win32::last_error("DiUninstallDevice", instance_id(set, data)).to_string().c_str());
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
        std::fprintf(stderr, "ERROR: %s\n", win32::last_error("SetupUninstallOEMInfW", win32::to_utf8(oem)).to_string().c_str());
        ++failures;
      }
    }
    if (n.value() == 0) {
      std::puts("no Tessera virtual display device");
    }
    return failures ? 1 : (reboot_needed ? 3 : 0);
  }

  int cmd_query() {
    devinfo_t set;
    set.h = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_TESSERA_VDD, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set.h == INVALID_HANDLE_VALUE) {
      return fail(win32::last_error("SetupDiGetClassDevsW(interface)"));
    }
    SP_DEVICE_INTERFACE_DATA iface {sizeof(SP_DEVICE_INTERFACE_DATA)};
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
    std::puts("Usage: vdd-install install <TesseraVdd.inf> | remove [--purge] | status | query");
  }

}  // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::wstring cmd = argv[1];
  if (cmd == L"install" && argc == 3) {
    return cmd_install(argv[2]);
  }
  if (cmd == L"remove" && (argc == 2 || (argc == 3 && std::wstring(argv[2]) == L"--purge"))) {
    return cmd_remove(argc == 3);
  }
  if (cmd == L"status" && argc == 2) {
    return cmd_status();
  }
  if (cmd == L"query" && argc == 2) {
    return cmd_query();
  }
  usage();
  return 2;
}
