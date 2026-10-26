/**
 * @file host-helpers/common/src/vdd_client.cpp
 * @brief Tessera VDD control-interface client (see vdd_client.h).
 */
// standard includes
#include <cstring>
#include <memory>

// platform includes
#include <windows.h>
#include <objbase.h>
#include <setupapi.h>

#include <initguid.h>  // GUID_DEVINTERFACE_TESSERA_VDD (selectany, so other translation units may define it too)

// local includes
#include "tessera/vdd_client.h"

namespace tessera::vdd {

  namespace {

    std::string ntstatus_message(std::int32_t status) {
      HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
      wchar_t *buffer = nullptr;
      const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, ntdll, static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
      std::string text;
      if (n != 0 && buffer != nullptr) {
        std::wstring w(buffer, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) {
          w.pop_back();
        }
        text = win32::to_utf8(w);
      }
      if (buffer != nullptr) {
        LocalFree(buffer);
      }
      return text.empty() ? "unknown NTSTATUS" : text;
    }

    /// Sends a request with a size-prefixed input and output struct.
    template<class In, class Out>
    status_t call(HANDLE h, DWORD code, const In *in, Out &out, const char *where) {
      DWORD got = 0;
      const DWORD in_size = in != nullptr ? static_cast<DWORD>(sizeof(In)) : 0;
      if (!DeviceIoControl(h, code, const_cast<In *>(in), in_size, &out, static_cast<DWORD>(sizeof(Out)), &got, nullptr)) {
        return win32::last_error(where);
      }
      if (got != sizeof(Out) || out.size != sizeof(Out)) {
        return make_error(tessera_code_e::invalid_data, where, "unexpected reply size " + std::to_string(got) + " (driver interface version mismatch?)");
      }
      return status_t::ok();
    }

    template<class T>
    std::unique_ptr<T> zeroed() {
      auto p = std::make_unique<T>();
      std::memset(p.get(), 0, sizeof(T));
      p->size = sizeof(T);
      return p;
    }

  }  // namespace

  error_t ntstatus_error(std::int32_t status, std::string where, std::string_view context) {
    std::string message = ntstatus_message(status);
    if (!context.empty()) {
      message += " [" + std::string(context) + "]";
    }
    return error_t {error_t::domain_e::ntstatus, static_cast<std::uint32_t>(status), std::move(where), std::move(message)};
  }

  result_t<monitor_id_t> random_monitor_id() {
    GUID g;
    if (const HRESULT hr = CoCreateGuid(&g); FAILED(hr)) {
      return win32::hresult_error(hr, "CoCreateGuid");
    }
    monitor_id_t id;
    static_assert(sizeof(g) == 16);
    std::memcpy(id.data(), &g, 16);
    return id;
  }

  result_t<vdd_client_t> vdd_client_t::open() {
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_TESSERA_VDD, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
      return win32::last_error("SetupDiGetClassDevsW(Tessera VDD)");
    }
    SP_DEVICE_INTERFACE_DATA data {};
    data.cbSize = sizeof(data);
    std::wstring path;
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_TESSERA_VDD, 0, &data)) {
      DWORD needed = 0;
      SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &needed, nullptr);
      std::vector<BYTE> buf(needed);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (needed != 0 && SetupDiGetDeviceInterfaceDetailW(set, &data, detail, needed, nullptr, nullptr)) {
        path = detail->DevicePath;
      }
    }
    SetupDiDestroyDeviceInfoList(set);
    if (path.empty()) {
      return make_error(tessera_code_e::not_found, "vdd_client_t::open", "no Tessera VDD control interface (driver not installed or not started)");
    }
    win32::unique_handle_t h(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h) {
      return win32::last_error("CreateFileW(Tessera VDD)", "needs SYSTEM or Administrators");
    }
    return vdd_client_t(std::move(h));
  }

  result_t<tessera_vdd_version_t> vdd_client_t::version() const {
    tessera_vdd_version_t v {};
    if (auto st = call<int>(handle_.get(), IOCTL_TESSERA_VDD_GET_VERSION, nullptr, v, "DeviceIoControl(GET_VERSION)"); !st) {
      return st.error();
    }
    if (v.interface_version != TESSERA_VDD_INTERFACE_VERSION) {
      return make_error(tessera_code_e::unsupported, "vdd_client_t::version", "driver interface " + std::to_string(v.interface_version) + ", expected " + std::to_string(TESSERA_VDD_INTERFACE_VERSION));
    }
    return v;
  }

  status_t vdd_client_t::set_render_adapter(const LUID &adapter) const {
    tessera_vdd_render_adapter_t in {};
    in.size = sizeof(in);
    in.luid_low = adapter.LowPart;
    in.luid_high = adapter.HighPart;
    DWORD got = 0;
    if (!DeviceIoControl(handle_.get(), IOCTL_TESSERA_VDD_SET_RENDER_ADAPTER, &in, sizeof(in), nullptr, 0, &got, nullptr)) {
      return win32::last_error("DeviceIoControl(SET_RENDER_ADAPTER)");
    }
    return status_t::ok();
  }

  result_t<std::vector<result_t<added_monitor_t>>> vdd_client_t::add(const std::vector<monitor_spec_t> &monitors) const {
    if (monitors.empty() || monitors.size() > TESSERA_VDD_MAX_MONITORS) {
      return make_error(tessera_code_e::invalid_data, "vdd_client_t::add", "1 to " + std::to_string(TESSERA_VDD_MAX_MONITORS) + " monitors per request");
    }
    auto in = zeroed<tessera_vdd_add_request_t>();
    in->count = static_cast<unsigned int>(monitors.size());
    for (std::size_t i = 0; i < monitors.size(); ++i) {
      const monitor_spec_t &s = monitors[i];
      tessera_vdd_monitor_t &m = in->monitors[i];
      std::memcpy(m.id, s.id.data(), 16);
      m.width = s.width;
      m.height = s.height;
      m.refresh_numerator = s.refresh_numerator;
      m.refresh_denominator = s.refresh_denominator;
      m.flags = s.hdr ? TESSERA_VDD_MONITOR_HDR : 0u;
      m.width_mm = s.width_mm;
      m.height_mm = s.height_mm;
      for (std::size_t c = 0; c < s.name.size() && c + 1 < TESSERA_VDD_NAME_CHARS; ++c) {
        m.name[c] = static_cast<unsigned short>(s.name[c]);
      }
    }
    auto out = zeroed<tessera_vdd_add_reply_t>();
    if (auto st = call(handle_.get(), IOCTL_TESSERA_VDD_ADD_MONITORS, in.get(), *out, "DeviceIoControl(ADD_MONITORS)"); !st) {
      return st.error();
    }
    std::vector<result_t<added_monitor_t>> results;
    for (std::size_t i = 0; i < monitors.size(); ++i) {
      const tessera_vdd_add_result_t &r = out->results[i];
      if (r.status < 0) {
        results.emplace_back(ntstatus_error(r.status, "ADD_MONITORS", "monitor " + std::to_string(i)));
        continue;
      }
      added_monitor_t a;
      a.id = monitors[i].id;
      a.target_id = r.target_id;
      a.adapter.LowPart = r.adapter_luid_low;
      a.adapter.HighPart = r.adapter_luid_high;
      a.reclaimed = (r.flags & TESSERA_VDD_ADDED_RECLAIMED) != 0;
      results.emplace_back(a);
    }
    return results;
  }

  result_t<std::vector<status_t>> vdd_client_t::remove(const std::vector<monitor_id_t> &ids) const {
    if (ids.empty() || ids.size() > TESSERA_VDD_MAX_MONITORS) {
      return make_error(tessera_code_e::invalid_data, "vdd_client_t::remove", "1 to " + std::to_string(TESSERA_VDD_MAX_MONITORS) + " monitors per request");
    }
    auto in = zeroed<tessera_vdd_remove_request_t>();
    in->count = static_cast<unsigned int>(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
      std::memcpy(in->ids[i], ids[i].data(), 16);
    }
    auto out = zeroed<tessera_vdd_remove_reply_t>();
    if (auto st = call(handle_.get(), IOCTL_TESSERA_VDD_REMOVE_MONITORS, in.get(), *out, "DeviceIoControl(REMOVE_MONITORS)"); !st) {
      return st.error();
    }
    std::vector<status_t> statuses;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (out->status[i] < 0) {
        statuses.emplace_back(ntstatus_error(out->status[i], "REMOVE_MONITORS", "monitor " + std::to_string(i)));
      } else {
        statuses.emplace_back(status_t::ok());
      }
    }
    return statuses;
  }

  result_t<std::vector<tessera_vdd_list_entry_t>> vdd_client_t::list() const {
    auto out = zeroed<tessera_vdd_list_reply_t>();
    if (auto st = call<int>(handle_.get(), IOCTL_TESSERA_VDD_LIST_MONITORS, nullptr, *out, "DeviceIoControl(LIST_MONITORS)"); !st) {
      return st.error();
    }
    const unsigned n = out->count <= TESSERA_VDD_MAX_MONITORS ? out->count : TESSERA_VDD_MAX_MONITORS;
    return std::vector<tessera_vdd_list_entry_t>(out->entries, out->entries + n);
  }

}  // namespace tessera::vdd
