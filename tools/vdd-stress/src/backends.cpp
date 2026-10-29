/**
 * @file tools/vdd-stress/src/backends.cpp
 * @brief Tessera, SudoVDA, parsec-vdd and MTT VDD backends for vdd-stress.
 *
 * Tessera's driver is driven through tessera/vdd_client.h. The third-party control interfaces are re-declared here
 * from the drivers' public sources (interface GUIDs, IOCTL codes,
 * buffer layouts); no third-party code is copied:
 *   SudoVDA     github.com/SudoMaker/SudoVDA  Common/Include/sudovda-ioctl.h (protocol 0.2.x)
 *   parsec-vdd  github.com/nomi-san/parsec-vdd core/parsec-vdd.h (driver 0.45)
 *   MTT VDD     github.com/VirtualDrivers/Virtual-Display-Driver (fixed count from vdd_settings.xml)
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <vector>

// platform includes
#include <windows.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <setupapi.h>
#include <winioctl.h>

// local includes
#include "backends.h"
#include "tessera/display_modes.h"
#include "tessera/vdd_client.h"
#include "tessera/win32.h"

namespace tessera::stress {

  namespace {

    constexpr GUID display_class = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

    error_t unsupported(const char *backend, const char *what) {
      return make_error(tessera_code_e::unsupported, backend, std::string(what) + " is not supported by this driver");
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

    /// Opens the first present device interface of the given class.
    result_t<win32::unique_handle_t> open_interface(const GUID &iface, DWORD flags, const char *backend) {
      devinfo_t set;
      set.h = SetupDiGetClassDevsW(&iface, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
      if (set.h == INVALID_HANDLE_VALUE) {
        return win32::last_error("SetupDiGetClassDevsW(interface)", backend);
      }
      SP_DEVICE_INTERFACE_DATA data {};
      data.cbSize = sizeof(data);
      for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set.h, nullptr, &iface, i, &data); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set.h, &data, nullptr, 0, &needed, nullptr);
        std::vector<BYTE> buf(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set.h, &data, detail, needed, nullptr, nullptr)) {
          continue;
        }
        win32::unique_handle_t h(CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | flags, nullptr));
        if (h) {
          return h;
        }
        return win32::last_error("CreateFileW(control interface)", std::string(backend) + " (needs Administrators)");
      }
      return make_error(tessera_code_e::not_found, backend, "no control interface: is the driver installed and started?");
    }

    /// Finds the device node with the given hardware ID (present or disabled).
    result_t<DEVINST> find_device(const wchar_t *hardware_id, const char *backend) {
      devinfo_t set;
      set.h = SetupDiGetClassDevsW(&display_class, nullptr, nullptr, 0);
      if (set.h == INVALID_HANDLE_VALUE) {
        return win32::last_error("SetupDiGetClassDevsW(display)", backend);
      }
      SP_DEVINFO_DATA data {};
      data.cbSize = sizeof(data);
      for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &data); ++i) {
        wchar_t ids[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids), sizeof(ids) - sizeof(wchar_t), nullptr)) {
          continue;
        }
        for (const wchar_t *p = ids; *p != L'\0'; p += wcslen(p) + 1) {
          if (_wcsicmp(p, hardware_id) == 0) {
            return data.DevInst;
          }
        }
      }
      return make_error(tessera_code_e::not_found, backend, "device " + win32::to_utf8(hardware_id) + " is not installed");
    }

    error_t cm_error(CONFIGRET cr, const char *where, const char *backend) {
      return make_error(tessera_code_e::invalid_data, where, std::string(backend) + ": CONFIGRET " + std::to_string(cr));
    }

    // ---------------------------------------------------------------------------------------------------------
    // SudoVDA
    // ---------------------------------------------------------------------------------------------------------

    constexpr GUID sudovda_interface = {0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};
    constexpr DWORD sudovda_ioctl_add = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD sudovda_ioctl_remove = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD sudovda_ioctl_ping = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);

    struct sudovda_add_in_t {
      UINT width;
      UINT height;
      UINT refresh;  ///< mHz; the driver multiplies values below 1000 by 1000.
      GUID monitor;
      CHAR device_name[14];
      CHAR serial[14];
    };

    struct sudovda_add_out_t {
      LUID adapter;
      UINT target_id;
    };

    struct sudovda_remove_in_t {
      GUID monitor;
    };

    static_assert(sizeof(sudovda_add_in_t) == 56);
    static_assert(sizeof(sudovda_add_out_t) == 12);

    class sudovda_t: public backend_t {
    public:
      const char *name() const override {
        return "sudovda";
      }

      bool dynamic() const override {
        return true;
      }

      // The driver removes every display if no IOCTL arrives for 3 s (its default watchdog).
      std::chrono::milliseconds keepalive_interval() const override {
        return std::chrono::milliseconds(1000);
      }

      status_t open() override {
        auto h = open_interface(sudovda_interface, 0, name());
        if (!h) {
          return h.error();
        }
        // A second handle for the keepalive thread, so pings are not queued behind a slow add on the same
        // synchronous file object.
        auto k = open_interface(sudovda_interface, 0, name());
        if (!k) {
          return k.error();
        }
        control_ = std::move(h.value());
        ping_ = std::move(k.value());
        return status_t::ok();
      }

      result_t<std::uint32_t> add(const display_mode_t &mode) override {
        sudovda_add_in_t in {};
        in.width = mode.width;
        in.height = mode.height;
        in.refresh = mode.refresh_mhz;
        if (const HRESULT hr = CoCreateGuid(&in.monitor); FAILED(hr)) {
          return win32::hresult_error(hr, "CoCreateGuid");
        }
        const std::uint32_t handle = next_++;
        std::snprintf(in.device_name, sizeof(in.device_name), "TsrStress%02u", handle % 100);
        std::snprintf(in.serial, sizeof(in.serial), "TSR%010u", handle);
        sudovda_add_out_t out {};
        DWORD got = 0;
        if (!DeviceIoControl(control_.get(), sudovda_ioctl_add, &in, sizeof(in), &out, sizeof(out), &got, nullptr)) {
          return win32::last_error("DeviceIoControl(SudoVDA ADD)", mode.to_string());
        }
        monitors_[handle] = in.monitor;
        return handle;
      }

      status_t remove(std::uint32_t handle) override {
        const auto it = monitors_.find(handle);
        if (it == monitors_.end()) {
          return make_error(tessera_code_e::not_found, "sudovda remove", "unknown handle " + std::to_string(handle));
        }
        sudovda_remove_in_t in {it->second};
        DWORD got = 0;
        if (!DeviceIoControl(control_.get(), sudovda_ioctl_remove, &in, sizeof(in), nullptr, 0, &got, nullptr)) {
          return win32::last_error("DeviceIoControl(SudoVDA REMOVE)", "handle " + std::to_string(handle));
        }
        monitors_.erase(it);
        return status_t::ok();
      }

      status_t keepalive() override {
        DWORD got = 0;
        if (!DeviceIoControl(ping_.get(), sudovda_ioctl_ping, nullptr, 0, nullptr, 0, &got, nullptr)) {
          return win32::last_error("DeviceIoControl(SudoVDA PING)");
        }
        return status_t::ok();
      }

    private:
      win32::unique_handle_t control_;
      win32::unique_handle_t ping_;
      std::map<std::uint32_t, GUID> monitors_;
      std::uint32_t next_ = 1;
    };

    // ---------------------------------------------------------------------------------------------------------
    // parsec-vdd
    // ---------------------------------------------------------------------------------------------------------

    constexpr GUID parsec_interface = {0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
    constexpr DWORD parsec_ioctl_add = 0x0022e004;
    constexpr DWORD parsec_ioctl_remove = 0x0022a008;
    constexpr DWORD parsec_ioctl_update = 0x0022a00c;

    class parsec_t: public backend_t {
    public:
      const char *name() const override {
        return "parsec";
      }

      bool dynamic() const override {
        return true;
      }

      // The driver unplugs every display unless it is updated at least every 100 ms.
      std::chrono::milliseconds keepalive_interval() const override {
        return std::chrono::milliseconds(50);
      }

      std::string notes() const override {
        return "parsec-vdd has no per-display mode in its IOCTLs; displays use the driver's default modes, so --mode is ignored";
      }

      status_t open() override {
        auto h = open_interface(parsec_interface, FILE_FLAG_OVERLAPPED, name());
        if (!h) {
          return h.error();
        }
        device_ = std::move(h.value());
        return status_t::ok();
      }

      result_t<std::uint32_t> add(const display_mode_t &) override {
        auto index = ioctl(parsec_ioctl_add, nullptr, 0, "ADD");
        if (!index) {
          return index.error();
        }
        if (auto st = keepalive(); !st) {
          return st.error();
        }
        return index.value();
      }

      status_t remove(std::uint32_t handle) override {
        // The display index travels as a 16-bit big-endian value.
        const BYTE be[2] = {static_cast<BYTE>((handle >> 8) & 0xff), static_cast<BYTE>(handle & 0xff)};
        if (auto r = ioctl(parsec_ioctl_remove, be, sizeof(be), "REMOVE"); !r) {
          return r.error();
        }
        return keepalive();
      }

      status_t keepalive() override {
        if (auto r = ioctl(parsec_ioctl_update, nullptr, 0, "UPDATE"); !r) {
          return r.error();
        }
        return status_t::ok();
      }

    private:
      result_t<std::uint32_t> ioctl(DWORD code, const void *data, std::size_t size, const char *what) {
        BYTE in[32] = {};
        if (data != nullptr) {
          std::memcpy(in, data, std::min(size, sizeof(in)));
        }
        DWORD out = 0;
        DWORD got = 0;
        OVERLAPPED ov {};
        win32::unique_handle_t ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!ev) {
          return win32::last_error("CreateEventW");
        }
        ov.hEvent = ev.get();
        const std::string where = std::string("DeviceIoControl(parsec ") + what + ")";
        if (!DeviceIoControl(device_.get(), code, in, sizeof(in), &out, sizeof(out), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
          return win32::last_error(where);
        }
        if (!GetOverlappedResultEx(device_.get(), &ov, &got, 5000, FALSE)) {
          auto e = win32::last_error(where, "waited 5 s");
          CancelIoEx(device_.get(), &ov);
          GetOverlappedResult(device_.get(), &ov, &got, TRUE);  // the OVERLAPPED must outlive the request
          return e;
        }
        return static_cast<std::uint32_t>(out);
      }

      win32::unique_handle_t device_;
    };

    // ---------------------------------------------------------------------------------------------------------
    // MTT VDD
    // ---------------------------------------------------------------------------------------------------------

    class mtt_t: public backend_t {
    public:
      explicit mtt_t(std::string config):
          config_(std::move(config)) {}

      const char *name() const override {
        return "mtt";
      }

      bool dynamic() const override {
        return false;
      }

      std::chrono::milliseconds keepalive_interval() const override {
        return std::chrono::milliseconds(0);
      }

      std::string notes() const override {
        return "MTT VDD creates a fixed number of displays at adapter start (vdd_settings.xml <count>); a count change "
               "restarts the device (disable + enable), and --mode only selects the preferred resolution from its list";
      }

      status_t open() override {
        auto dev = find_device(L"Root\\MttVDD", name());
        if (!dev) {
          return dev.error();
        }
        devinst_ = dev.value();
        // Start from zero displays: the device brings up its configured count as soon as it is installed.
        return set_count(0, {});
      }

      status_t set_count(std::uint32_t count, const display_mode_t &) override {
        // Stop the device first: MTT reads its settings only when the adapter starts.
        if (const CONFIGRET cr = CM_Disable_DevNode(devinst_, CM_DISABLE_UI_NOT_OK); cr != CR_SUCCESS) {
          return cm_error(cr, "CM_Disable_DevNode", name());
        }
        if (count == 0) {
          return status_t::ok();
        }
        if (auto st = write_count(count); !st) {
          return st;
        }
        if (const CONFIGRET cr = CM_Enable_DevNode(devinst_, 0); cr != CR_SUCCESS) {
          return cm_error(cr, "CM_Enable_DevNode", name());
        }
        return status_t::ok();
      }

    private:
      status_t write_count(std::uint32_t count) {
        const std::filesystem::path path(win32::to_wide(config_));
        std::string text;
        {
          std::ifstream in(path, std::ios::binary);
          if (!in) {
            return make_error(tessera_code_e::not_found, "mtt", "cannot read " + config_);
          }
          text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        static const std::regex count_re(R"(<count>\s*\d+\s*</count>)");
        if (!std::regex_search(text, count_re)) {
          return make_error(tessera_code_e::invalid_data, "mtt", "no <count> element in " + config_);
        }
        text = std::regex_replace(text, count_re, "<count>" + std::to_string(count) + "</count>", std::regex_constants::format_first_only);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
          return make_error(tessera_code_e::invalid_data, "mtt", "cannot write " + config_);
        }
        return status_t::ok();
      }

      std::string config_;
      DEVINST devinst_ = 0;
    };

    // ---------------------------------------------------------------------------------------------------------
    // Tessera VDD
    // ---------------------------------------------------------------------------------------------------------

    class tessera_t: public backend_t {
    public:
      explicit tessera_t(const backend_options_t &options):
          hdr_(options.hdr),
          new_ids_(options.new_ids) {}

      const char *name() const override {
        return "tessera";
      }

      bool dynamic() const override {
        return true;
      }

      // Monitors belong to this process's handle; no keepalive.
      std::chrono::milliseconds keepalive_interval() const override {
        return std::chrono::milliseconds(0);
      }

      bool supports_batch() const override {
        return true;
      }

      status_t open() override {
        auto c = vdd::vdd_client_t::open();
        if (!c) {
          return c.error();
        }
        auto v = c.value().version();
        if (!v) {
          return v.error();
        }
        client_.emplace(std::move(c.value()));
        return status_t::ok();
      }

      result_t<std::uint32_t> add(const display_mode_t &mode) override {
        std::vector<std::uint32_t> handles;
        if (auto st = add_batch(mode, 1, handles); !st) {
          return st.error();
        }
        return handles.front();
      }

      status_t remove(std::uint32_t handle) override {
        return remove_batch({handle});
      }

      status_t add_batch(const display_mode_t &mode, std::uint32_t count, std::vector<std::uint32_t> &handles) override {
        std::vector<vdd::monitor_spec_t> specs;
        std::vector<std::uint32_t> wanted;
        std::vector<std::uint32_t> slots;
        for (std::uint32_t i = 0; i < count; ++i) {
          vdd::monitor_spec_t s;
          if (new_ids_) {
            auto id = vdd::random_monitor_id();
            if (!id) {
              return id.error();
            }
            s.id = id.value();
          } else {
            const auto slot = free_slot(slots);
            if (!slot) {
              return make_error(tessera_code_e::invalid_data, "tessera add", "more than " + std::to_string(TESSERA_VDD_MAX_MONITORS) + " displays");
            }
            slots.push_back(*slot);
            s.id = stable_id(*slot);
          }
          s.hdr = hdr_;
          s.width = mode.width;
          s.height = mode.height;
          to_rational(mode.refresh_mhz, s.refresh_numerator, s.refresh_denominator);
          const std::uint32_t handle = next_++;
          s.name = L"Stress " + std::to_wstring(handle);
          specs.push_back(s);
          wanted.push_back(handle);
        }
        auto results = client_->add(specs);
        if (!results) {
          return results.error();
        }
        status_t first_failure;
        for (std::size_t i = 0; i < specs.size(); ++i) {
          if (results.value()[i]) {
            const vdd::added_monitor_t &a = results.value()[i].value();
            ids_[wanted[i]] = specs[i].id;
            targets_[wanted[i]] = {a.adapter.LowPart, a.adapter.HighPart, a.target_id};
            if (!new_ids_) {
              slots_[wanted[i]] = slots[i];
            }
            handles.push_back(wanted[i]);
          } else if (first_failure) {
            first_failure = results.value()[i].error();
          }
        }
        return first_failure;
      }

      status_t remove_batch(const std::vector<std::uint32_t> &handles) override {
        std::vector<vdd::monitor_id_t> ids;
        for (const auto h : handles) {
          const auto it = ids_.find(h);
          if (it == ids_.end()) {
            return make_error(tessera_code_e::not_found, "tessera remove", "unknown handle " + std::to_string(h));
          }
          ids.push_back(it->second);
        }
        auto statuses = client_->remove(ids);
        if (!statuses) {
          return statuses.error();
        }
        status_t first_failure;
        for (std::size_t i = 0; i < handles.size(); ++i) {
          ids_.erase(handles[i]);
          targets_.erase(handles[i]);
          slots_.erase(handles[i]);
          if (!statuses.value()[i] && first_failure) {
            first_failure = statuses.value()[i].error();
          }
        }
        return first_failure;
      }

      std::optional<target_ref_t> target_of(std::uint32_t handle) const override {
        const auto it = targets_.find(handle);
        if (it == targets_.end()) {
          return std::nullopt;
        }
        return it->second;
      }

    private:
      /// The lowest slot that neither an added display nor `taken` uses.
      std::optional<std::uint32_t> free_slot(const std::vector<std::uint32_t> &taken) const {
        for (std::uint32_t s = 0; s < TESSERA_VDD_MAX_MONITORS; ++s) {
          const auto in_use = [s](const auto &e) {
            return e.second == s;
          };
          if (std::find(taken.begin(), taken.end(), s) == taken.end() && std::none_of(slots_.begin(), slots_.end(), in_use)) {
            return s;
          }
        }
        return std::nullopt;
      }

      /**
       * @brief The same ID for a slot in every run, as a host gives each client monitor a stable ID, so Windows
       *        sees the same few monitors again instead of a new monitor (device node and saved settings) per add.
       *        Chosen so that the driver puts slot s on connector s, whatever order the slots are added in.
       */
      static vdd::monitor_id_t stable_id(std::uint32_t slot) {
        static constexpr char prefix[] = "vdd-stress";
        std::uint8_t raw[16] = {};
        std::memcpy(raw, prefix, sizeof(prefix) - 1);
        raw[10] = static_cast<std::uint8_t>(slot);
        for (std::uint32_t n = 0;; ++n) {
          std::memcpy(raw + 12, &n, sizeof(n));
          if (modes::identity_from_id(raw).serial % TESSERA_VDD_MAX_MONITORS == slot) {
            break;
          }
        }
        vdd::monitor_id_t id;
        std::memcpy(id.data(), raw, sizeof(raw));
        return id;
      }

      /// 59.94-style rates become exact NTSC fractions (60000/1001); others are millihertz over 1000.
      static void to_rational(std::uint32_t mhz, std::uint32_t &num, std::uint32_t &den) {
        for (const std::uint32_t hz : {24u, 30u, 48u, 60u, 72u, 90u, 120u, 144u, 240u}) {
          if (mhz == hz * 1000) {
            num = hz;
            den = 1;
            return;
          }
          if (mhz == static_cast<std::uint32_t>(std::lround(hz * 1000000.0 / 1001.0))) {
            num = hz * 1000;
            den = 1001;
            return;
          }
        }
        num = mhz;
        den = 1000;
      }

      bool hdr_;
      bool new_ids_;
      std::optional<vdd::vdd_client_t> client_;
      std::map<std::uint32_t, vdd::monitor_id_t> ids_;
      std::map<std::uint32_t, target_ref_t> targets_;
      std::map<std::uint32_t, std::uint32_t> slots_;  ///< handle -> slot of its stable ID
      std::uint32_t next_ = 1;
    };

  }  // namespace

  std::string display_mode_t::to_string() const {
    char buf[64];
    if (refresh_mhz % 1000 == 0) {
      std::snprintf(buf, sizeof(buf), "%ux%u@%u", width, height, refresh_mhz / 1000);
    } else {
      std::snprintf(buf, sizeof(buf), "%ux%u@%.3f", width, height, refresh_mhz / 1000.0);
    }
    return buf;
  }

  result_t<display_mode_t> parse_mode(const std::string &text) {
    static const std::regex re(R"((\d{3,5})x(\d{3,5})@(\d{2,3}(?:\.\d{1,3})?))");
    std::smatch m;
    if (!std::regex_match(text, m, re)) {
      return make_error(tessera_code_e::invalid_data, "parse_mode", "expected WIDTHxHEIGHT@HZ, got '" + text + "'");
    }
    display_mode_t mode;
    mode.width = static_cast<std::uint32_t>(std::stoul(m[1]));
    mode.height = static_cast<std::uint32_t>(std::stoul(m[2]));
    mode.refresh_mhz = static_cast<std::uint32_t>(std::lround(std::stod(m[3]) * 1000.0));
    if (mode.width > 16384 || mode.height > 16384 || mode.refresh_mhz < 23000 || mode.refresh_mhz > 500000) {
      return make_error(tessera_code_e::invalid_data, "parse_mode", "mode out of range: " + text);
    }
    return mode;
  }

  result_t<std::uint32_t> backend_t::add(const display_mode_t &) {
    return unsupported(name(), "add");
  }

  status_t backend_t::remove(std::uint32_t) {
    return unsupported(name(), "remove");
  }

  status_t backend_t::set_count(std::uint32_t, const display_mode_t &) {
    return unsupported(name(), "set_count");
  }

  status_t backend_t::add_batch(const display_mode_t &, std::uint32_t, std::vector<std::uint32_t> &) {
    return unsupported(name(), "add_batch");
  }

  status_t backend_t::remove_batch(const std::vector<std::uint32_t> &) {
    return unsupported(name(), "remove_batch");
  }

  result_t<std::unique_ptr<backend_t>> make_backend(const std::string &name, const backend_options_t &options) {
    if (name == "tessera") {
      return std::unique_ptr<backend_t>(std::make_unique<tessera_t>(options));
    }
    if (name == "sudovda") {
      return std::unique_ptr<backend_t>(std::make_unique<sudovda_t>());
    }
    if (name == "parsec") {
      return std::unique_ptr<backend_t>(std::make_unique<parsec_t>());
    }
    if (name == "mtt") {
      return std::unique_ptr<backend_t>(std::make_unique<mtt_t>(options.mtt_config));
    }
    return make_error(tessera_code_e::invalid_data, "make_backend", "unknown backend '" + name + "' (tessera, sudovda, parsec, mtt)");
  }

}  // namespace tessera::stress
