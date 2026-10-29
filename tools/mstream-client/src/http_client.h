/**
 * @file tools/mstream-client/src/http_client.h
 * @brief Minimal HTTP and HTTPS (client certificate, pinned server certificate) GET for the GameStream API.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <string_view>

namespace mstream {

  /// The client identity used for HTTPS and pairing (PEM).
  struct identity_t {
    std::string unique_id;  ///< 16 hex characters
    std::string cert_pem;
    std::string key_pem;
    std::string server_cert_pem;  ///< Pinned after pairing; empty before.
  };

  /// Loads the identity from `dir`, creating a new certificate and key if there is none.
  std::optional<identity_t> load_or_create_identity(const std::string &dir);

  /// Saves the pinned server certificate.
  bool save_server_cert(const std::string &dir, const std::string &pem);

  /**
   * @brief GET http(s)://host:port/path?query and return the body.
   * @param tls Use HTTPS with the identity's client certificate; the server must present `identity.server_cert_pem`
   *            when it is set.
   */
  std::optional<std::string> http_get(const std::string &host, int port, bool tls, const std::string &path_and_query, const identity_t &identity, int timeout_ms);

  /// Text of the first <tag>…</tag> in an XML reply.
  std::optional<std::string> xml_value(std::string_view xml, std::string_view tag);

  /// The status_code attribute of the reply's root element (200 if absent).
  int xml_status(std::string_view xml);

  std::string to_hex(std::string_view bytes);
  std::string from_hex(std::string_view hex);

}  // namespace mstream
