/**
 * @file host-helpers/common/include/tessera/result.h
 * @brief Small error and result types for Tessera's native components.
 *
 * Library APIs report failures through these types instead of throwing, so errors can cross thread,
 * process (via IPC) and toolchain (MinGW/MSVC) boundaries as plain data.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace tessera {

  /**
   * @brief Describes a failed operation.
   */
  struct error_t {
    enum class domain_e {
      win32,  ///< `code` is a Win32 error code (GetLastError()).
      hresult,  ///< `code` is an HRESULT.
      ntstatus,  ///< `code` is an NTSTATUS.
      tessera,  ///< Tessera-level failure (validation, protocol, ...); `code` is one of `tessera_code_e`.
    };

    domain_e domain = domain_e::tessera;
    std::uint32_t code = 0;
    std::string where;  ///< The function or step that failed, e.g. "ReplaceFileW".
    std::string message;  ///< Formatted system message plus context (paths, display IDs, client IDs).

    /// One-line description: "where: message (domain 0x...)".
    std::string to_string() const;
  };

  /**
   * @brief Codes for `error_t::domain_e::tessera`.
   */
  enum class tessera_code_e : std::uint32_t {
    invalid_data = 1,  ///< Input failed validation (bad JSON, bad checksum, out-of-range field).
    not_found = 2,
    timeout = 3,
    too_large = 4,
    unsupported = 5,
    unsafe = 6,  ///< Refused because the result would leave the system in an unsafe state.
  };

  /// Builds a Tessera-domain error.
  inline error_t make_error(tessera_code_e code, std::string where, std::string message) {
    return error_t {error_t::domain_e::tessera, static_cast<std::uint32_t>(code), std::move(where), std::move(message)};
  }

  /**
   * @brief Either a value or an error. A minimal stand-in for C++23 std::expected.
   */
  template<class T>
  class result_t {
  public:
    result_t(T value):
        v_(std::move(value)) {}

    result_t(error_t error):
        v_(std::move(error)) {}

    bool has_value() const {
      return v_.index() == 0;
    }

    explicit operator bool() const {
      return has_value();
    }

    T &value() & {
      return std::get<0>(v_);
    }

    const T &value() const & {
      return std::get<0>(v_);
    }

    T &&value() && {
      return std::get<0>(std::move(v_));
    }

    T *operator->() {
      return &std::get<0>(v_);
    }

    const T *operator->() const {
      return &std::get<0>(v_);
    }

    const error_t &error() const {
      return std::get<1>(v_);
    }

  private:
    std::variant<T, error_t> v_;
  };

  /**
   * @brief Success or an error, for operations without a return value.
   */
  class status_t {
  public:
    status_t() = default;

    status_t(error_t error):
        e_(std::move(error)) {}

    static status_t ok() {
      return {};
    }

    bool has_value() const {
      return !e_.has_value();
    }

    explicit operator bool() const {
      return has_value();
    }

    const error_t &error() const {
      return *e_;
    }

  private:
    std::optional<error_t> e_;
  };

}  // namespace tessera
