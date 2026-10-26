/**
 * @file host-helpers/common/src/win32.cpp
 * @brief Implementation of the shared Win32 helpers.
 */
// local includes
#include "tessera/win32.h"

// standard includes
#include <cstdio>

// platform includes
#include <knownfolders.h>
#include <shlobj.h>

namespace tessera {

  std::string error_t::to_string() const {
    const char *d = "tessera";
    switch (domain) {
      case domain_e::win32:
        d = "win32";
        break;
      case domain_e::hresult:
        d = "hresult";
        break;
      case domain_e::ntstatus:
        d = "ntstatus";
        break;
      case domain_e::tessera:
        break;
    }
    char code_buf[32];
    std::snprintf(code_buf, sizeof(code_buf), "0x%08lX", static_cast<unsigned long>(code));
    return where + ": " + message + " (" + d + " " + code_buf + ")";
  }

}  // namespace tessera

namespace tessera::win32 {

  std::string to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
      return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
      return {};
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
  }

  std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) {
      return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
      return {};
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
  }

  std::string format_message(std::uint32_t code) {
    wchar_t *buf = nullptr;
    const DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      code,
      0,
      reinterpret_cast<wchar_t *>(&buf),
      0,
      nullptr
    );
    std::string msg;
    if (n > 0 && buf != nullptr) {
      msg = to_utf8(std::wstring_view(buf, n));
      while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ' || msg.back() == '.')) {
        msg.pop_back();
      }
    }
    if (buf != nullptr) {
      LocalFree(buf);
    }
    if (msg.empty()) {
      msg = "unknown error";
    }
    return msg;
  }

  namespace {
    std::string with_context(std::string msg, std::string_view context) {
      if (!context.empty()) {
        msg += " [";
        msg += context;
        msg += "]";
      }
      return msg;
    }
  }  // namespace

  error_t win32_error(DWORD code, std::string where, std::string_view context) {
    return error_t {error_t::domain_e::win32, code, std::move(where), with_context(format_message(code), context)};
  }

  error_t last_error(std::string where, std::wstring_view context) {
    const DWORD code = GetLastError();  // before the conversion below can change it
    return win32_error(code, std::move(where), to_utf8(context));
  }

  error_t last_error(std::string where, std::string_view context) {
    return win32_error(GetLastError(), std::move(where), context);
  }

  error_t hresult_error(HRESULT hr, std::string where, std::string_view context) {
    return error_t {error_t::domain_e::hresult, static_cast<std::uint32_t>(hr), std::move(where), with_context(format_message(static_cast<std::uint32_t>(hr)), context)};
  }

  result_t<std::filesystem::path> program_data_dir() {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr)) {
      if (raw != nullptr) {
        CoTaskMemFree(raw);
      }
      return hresult_error(hr, "SHGetKnownFolderPath", "FOLDERID_ProgramData");
    }
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    return p;
  }

  bool is_elevated() {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) {
      return false;
    }
    unique_handle_t token(token_raw);
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
      return false;
    }
    return elevation.TokenIsElevated != 0;
  }

}  // namespace tessera::win32
