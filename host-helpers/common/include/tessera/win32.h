/**
 * @file host-helpers/common/include/tessera/win32.h
 * @brief Win32 helpers shared by MinGW and MSVC builds: error formatting, UTF-8/UTF-16 conversion,
 *        RAII handle wrapper, and known-folder lookup.
 *
 * WIL is not used here on purpose: this code is compiled into the MinGW-built host as well.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

// platform includes
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// local includes
#include "tessera/result.h"

namespace tessera::win32 {

  /// Converts UTF-16 to UTF-8. Invalid sequences are replaced (never throws).
  std::string to_utf8(std::wstring_view wide);

  /// Converts UTF-8 to UTF-16. Invalid sequences are replaced (never throws).
  std::wstring to_wide(std::string_view utf8);

  /// Returns the system message for a Win32 error or HRESULT, without the trailing newline.
  std::string format_message(std::uint32_t code);

  /// Builds an error from GetLastError(). Call it immediately after the failing API.
  error_t last_error(std::string where, std::string_view context = {});

  /// Builds an error from an explicit Win32 error code.
  error_t win32_error(DWORD code, std::string where, std::string_view context = {});

  /// Builds an error from an HRESULT.
  error_t hresult_error(HRESULT hr, std::string where, std::string_view context = {});

  /**
   * @brief Owns a kernel HANDLE. Treats both nullptr and INVALID_HANDLE_VALUE as empty.
   */
  class unique_handle_t {
  public:
    unique_handle_t() = default;

    explicit unique_handle_t(HANDLE h):
        h_(h) {}

    ~unique_handle_t() {
      reset();
    }

    unique_handle_t(const unique_handle_t &) = delete;
    unique_handle_t &operator=(const unique_handle_t &) = delete;

    unique_handle_t(unique_handle_t &&other) noexcept:
        h_(std::exchange(other.h_, nullptr)) {}

    unique_handle_t &operator=(unique_handle_t &&other) noexcept {
      if (this != &other) {
        reset(std::exchange(other.h_, nullptr));
      }
      return *this;
    }

    HANDLE get() const {
      return h_;
    }

    bool valid() const {
      return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
    }

    explicit operator bool() const {
      return valid();
    }

    void reset(HANDLE h = nullptr) {
      if (valid()) {
        CloseHandle(h_);
      }
      h_ = h;
    }

    HANDLE release() {
      return std::exchange(h_, nullptr);
    }

  private:
    HANDLE h_ = nullptr;
  };

  /// Returns %ProgramData% (FOLDERID_ProgramData).
  result_t<std::filesystem::path> program_data_dir();

  /// True if the current process token is elevated (or SYSTEM).
  bool is_elevated();

}  // namespace tessera::win32
