/**
 * @file src/tessera/client_permissions.h
 * @brief [fork] Tessera: per-paired-client permissions for protocol extensions (brief §4.3).
 * @details Stored in tessera_clients.json next to Sunshine's own state, keyed by the client's UUID, so Sunshine's
 *          state format stays as upstream has it. A client without an entry gets the defaults.
 */
#pragma once

// standard includes
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace tessera::host::permissions {

  /// Whether the paired client with this certificate (PEM) may use multi-display streaming. Default: yes.
  bool multistream_allowed(const std::string &client_cert);

  /// Sets the multi-display permission of a paired client. false if the UUID names no paired client.
  bool set_multistream(const std::string &uuid, bool allowed);

  /// Adds each client's permissions (`tessera_multistream`) to Sunshine's client list (nvhttp::get_all_clients()).
  void annotate(nlohmann::json &named_certs);

}  // namespace tessera::host::permissions
