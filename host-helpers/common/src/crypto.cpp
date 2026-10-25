/**
 * @file host-helpers/common/src/crypto.cpp
 * @brief SHA-256 and random bytes via Windows CNG.
 */
// local includes
#include "tessera/crypto.h"
#include "tessera/win32.h"

// platform includes
#include <bcrypt.h>

namespace tessera::crypto {

  namespace {
    error_t ntstatus_error(NTSTATUS status, std::string where) {
      return error_t {error_t::domain_e::ntstatus, static_cast<std::uint32_t>(status), std::move(where), "CNG call failed"};
    }

    constexpr bool nt_success(NTSTATUS s) {
      return s >= 0;
    }
  }  // namespace

  std::string to_hex(const std::uint8_t *data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
      out.push_back(digits[data[i] >> 4]);
      out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
  }

  result_t<std::string> sha256_hex(std::string_view data) {
    std::array<std::uint8_t, 32> digest {};
    // BCRYPT_SHA256_ALG_HANDLE is a pseudo-handle (Windows 10+), so no provider needs opening or closing.
    const NTSTATUS s = BCryptHash(
      BCRYPT_SHA256_ALG_HANDLE,
      nullptr,
      0,
      reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
      static_cast<ULONG>(data.size()),
      digest.data(),
      static_cast<ULONG>(digest.size())
    );
    if (!nt_success(s)) {
      return ntstatus_error(s, "BCryptHash(SHA256)");
    }
    return to_hex(digest.data(), digest.size());
  }

  status_t random_bytes(void *out, std::size_t size) {
    const NTSTATUS s = BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!nt_success(s)) {
      return ntstatus_error(s, "BCryptGenRandom");
    }
    return status_t::ok();
  }

}  // namespace tessera::crypto
