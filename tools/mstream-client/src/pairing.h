/**
 * @file tools/mstream-client/src/pairing.h
 * @brief GameStream pairing (PIN), as Moonlight does it.
 */
#pragma once

// standard includes
#include <string>

// local includes
#include "http_client.h"

namespace mstream {

  /**
   * @brief Pairs with the host using `pin` (entered on the host while this waits) and pins its certificate.
   * @return An empty string on success, otherwise what failed.
   */
  std::string pair(const std::string &host, const std::string &identity_dir, identity_t &identity, const std::string &pin, const std::string &device_name);

}  // namespace mstream
