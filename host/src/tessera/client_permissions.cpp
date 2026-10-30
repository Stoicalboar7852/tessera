/**
 * @file src/tessera/client_permissions.cpp
 * @brief [fork] Tessera: per-paired-client permissions for protocol extensions.
 */
// standard includes
#include <filesystem>
#include <fstream>
#include <mutex>

// local includes
#include "client_permissions.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace tessera::host::permissions {

  namespace {
    std::mutex mutex;

    std::filesystem::path file() {
      return platf::appdata() / "tessera_clients.json";
    }

    /// { "<uuid>": { "multistream": bool } }
    nlohmann::json load() {
      std::ifstream in(file());
      if (!in) {
        return nlohmann::json::object();
      }
      try {
        auto json = nlohmann::json::parse(in);
        return json.is_object() ? json : nlohmann::json::object();
      } catch (const nlohmann::json::exception &e) {
        BOOST_LOG(warning) << "tessera_clients.json is unreadable, using defaults: "sv << e.what();
        return nlohmann::json::object();
      }
    }

    bool save(const nlohmann::json &json) {
      const auto path = file();
      const auto temp = path.string() + ".tmp";
      {
        std::ofstream out(temp, std::ios::trunc);
        out << json.dump(2);
        if (!out) {
          return false;
        }
      }
      std::error_code ec;
      std::filesystem::rename(temp, path, ec);
      return !ec;
    }

    bool allowed(const nlohmann::json &entries, const std::string &uuid) {
      const auto it = entries.find(uuid);
      return it == entries.end() || !it->is_object() || it->value("multistream", true);
    }
  }  // namespace

  bool multistream_allowed(const std::string &client_cert) {
    std::lock_guard lock {mutex};
    const auto entries = load();
    for (const auto &client : nvhttp::get_all_clients()) {
      const auto uuid = client.value("uuid", ""s);
      if (!uuid.empty() && nvhttp::get_cert_by_uuid(uuid) == client_cert) {
        return allowed(entries, uuid);
      }
    }
    return true;
  }

  bool set_multistream(const std::string &uuid, bool value) {
    if (uuid.empty() || nvhttp::get_cert_by_uuid(uuid).empty()) {
      return false;
    }
    std::lock_guard lock {mutex};
    auto entries = load();
    entries[uuid]["multistream"] = value;
    if (!save(entries)) {
      BOOST_LOG(error) << "Couldn't save tessera_clients.json"sv;
      return false;
    }
    BOOST_LOG(info) << "Client "sv << uuid << ": multi-display streaming "sv << (value ? "allowed"sv : "not allowed"sv);
    return true;
  }

  void annotate(nlohmann::json &named_certs) {
    std::lock_guard lock {mutex};
    const auto entries = load();
    for (auto &client : named_certs) {
      client["tessera_multistream"] = allowed(entries, client.value("uuid", ""s));
    }
  }

}  // namespace tessera::host::permissions
