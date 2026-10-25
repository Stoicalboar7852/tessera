/**
 * @file host-helpers/common/include/tessera/crypto.h
 * @brief SHA-256 and random bytes through Windows CNG (bcrypt), available to both MinGW and MSVC.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// local includes
#include "tessera/result.h"

namespace tessera::crypto {

  /// Lowercase hex SHA-256 of `data`.
  result_t<std::string> sha256_hex(std::string_view data);

  /// Fills `out` with cryptographically random bytes.
  status_t random_bytes(void *out, std::size_t size);

  /// Lowercase hex encoding.
  std::string to_hex(const std::uint8_t *data, std::size_t size);

}  // namespace tessera::crypto
