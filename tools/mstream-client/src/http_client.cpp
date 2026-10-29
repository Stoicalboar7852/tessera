/**
 * @file tools/mstream-client/src/http_client.cpp
 * @brief Minimal HTTP/HTTPS GET with OpenSSL, and the client identity (self-signed RSA certificate).
 */
// platform includes
#include <winsock2.h>
#include <ws2tcpip.h>

// standard includes
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>

// lib includes
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

// local includes
#include "http_client.h"

namespace mstream {

  namespace {
    template<class T, void (*F)(T *)>
    struct deleter_t {
      void operator()(T *p) const {
        F(p);
      }
    };

    using x509_t = std::unique_ptr<X509, deleter_t<X509, X509_free>>;
    using pkey_t = std::unique_ptr<EVP_PKEY, deleter_t<EVP_PKEY, EVP_PKEY_free>>;
    using bio_t = std::unique_ptr<BIO, deleter_t<BIO, BIO_free_all>>;
    using ssl_ctx_t = std::unique_ptr<SSL_CTX, deleter_t<SSL_CTX, SSL_CTX_free>>;
    using ssl_t = std::unique_ptr<SSL, deleter_t<SSL, SSL_free>>;

    std::string read_file(const std::filesystem::path &path) {
      std::ifstream in(path, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    bool write_file(const std::filesystem::path &path, const std::string &data) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << data;
      return static_cast<bool>(out);
    }

    std::string bio_string(BIO *bio) {
      char *data = nullptr;
      const auto length = BIO_get_mem_data(bio, &data);
      return std::string(data, static_cast<std::size_t>(length));
    }

    /// A self-signed RSA-2048 certificate, as Moonlight clients use.
    bool create_certificate(std::string &cert_pem, std::string &key_pem) {
      pkey_t key {EVP_RSA_gen(2048)};
      x509_t cert {X509_new()};
      if (!key || !cert) {
        return false;
      }
      X509_set_version(cert.get(), 2);
      ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 0);
      X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
      X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 365 * 20);
      X509_set_pubkey(cert.get(), key.get());
      auto *name = X509_get_subject_name(cert.get());
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Tessera mstream-client"), -1, -1, 0);
      X509_set_issuer_name(cert.get(), name);
      if (!X509_sign(cert.get(), key.get(), EVP_sha256())) {
        return false;
      }

      bio_t cert_bio {BIO_new(BIO_s_mem())};
      bio_t key_bio {BIO_new(BIO_s_mem())};
      PEM_write_bio_X509(cert_bio.get(), cert.get());
      PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
      cert_pem = bio_string(cert_bio.get());
      key_pem = bio_string(key_bio.get());
      return true;
    }

    x509_t parse_cert(const std::string &pem) {
      bio_t bio {BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
      return x509_t {PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
    }

    struct socket_t {
      SOCKET s = INVALID_SOCKET;

      ~socket_t() {
        if (s != INVALID_SOCKET) {
          closesocket(s);
        }
      }
    };

    bool connect_to(socket_t &sock, const std::string &host, int port, int timeout_ms) {
      addrinfo hints {};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo *result = nullptr;
      if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        return false;
      }
      for (auto *ai = result; ai != nullptr; ai = ai->ai_next) {
        sock.s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock.s == INVALID_SOCKET) {
          continue;
        }
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        if (connect(sock.s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
          freeaddrinfo(result);
          return true;
        }
        closesocket(sock.s);
        sock.s = INVALID_SOCKET;
      }
      freeaddrinfo(result);
      return false;
    }

    std::optional<std::string> body_of(const std::string &response) {
      const auto end = response.find("\r\n\r\n");
      if (end == std::string::npos || response.rfind("HTTP/1.", 0) != 0) {
        return std::nullopt;
      }
      return response.substr(end + 4);
    }
  }  // namespace

  std::optional<identity_t> load_or_create_identity(const std::string &dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    identity_t identity;
    const fs::path base {dir};

    if (fs::exists(base / "client.pem") && fs::exists(base / "client.key") && fs::exists(base / "uniqueid.txt")) {
      identity.cert_pem = read_file(base / "client.pem");
      identity.key_pem = read_file(base / "client.key");
      identity.unique_id = read_file(base / "uniqueid.txt");
    } else {
      if (!create_certificate(identity.cert_pem, identity.key_pem)) {
        return std::nullopt;
      }
      unsigned char raw[8];
      RAND_bytes(raw, sizeof(raw));
      identity.unique_id = to_hex(std::string_view(reinterpret_cast<const char *>(raw), sizeof(raw)));
      if (!write_file(base / "client.pem", identity.cert_pem) || !write_file(base / "client.key", identity.key_pem) ||
          !write_file(base / "uniqueid.txt", identity.unique_id)) {
        return std::nullopt;
      }
    }
    if (fs::exists(base / "server.pem")) {
      identity.server_cert_pem = read_file(base / "server.pem");
    }
    return identity;
  }

  bool save_server_cert(const std::string &dir, const std::string &pem) {
    return write_file(std::filesystem::path {dir} / "server.pem", pem);
  }

  std::optional<std::string> http_get(const std::string &host, int port, bool tls, const std::string &path_and_query, const identity_t &identity, int timeout_ms) {
    static const bool wsa = [] {
      WSADATA data;
      return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!wsa) {
      return std::nullopt;
    }

    socket_t sock;
    if (!connect_to(sock, host, port, timeout_ms)) {
      return std::nullopt;
    }
    const auto request = "GET " + path_and_query + " HTTP/1.1\r\nHost: " + host + ':' + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    std::string response;
    char buffer[16384];

    if (!tls) {
      if (send(sock.s, request.data(), static_cast<int>(request.size()), 0) != static_cast<int>(request.size())) {
        return std::nullopt;
      }
      int n;
      while ((n = recv(sock.s, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(n));
      }
      return body_of(response);
    }

    ssl_ctx_t ctx {SSL_CTX_new(TLS_client_method())};
    auto cert = parse_cert(identity.cert_pem);
    bio_t key_bio {BIO_new_mem_buf(identity.key_pem.data(), static_cast<int>(identity.key_pem.size()))};
    pkey_t key {PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr)};
    if (!ctx || !cert || !key || SSL_CTX_use_certificate(ctx.get(), cert.get()) != 1 || SSL_CTX_use_PrivateKey(ctx.get(), key.get()) != 1) {
      return std::nullopt;
    }
    // The host's certificate is self-signed; it is checked against the one pinned at pairing below
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

    ssl_t ssl {SSL_new(ctx.get())};
    SSL_set_fd(ssl.get(), static_cast<int>(sock.s));
    if (SSL_connect(ssl.get()) != 1) {
      return std::nullopt;
    }
    if (!identity.server_cert_pem.empty()) {
      x509_t peer {SSL_get1_peer_certificate(ssl.get())};
      auto pinned = parse_cert(identity.server_cert_pem);
      if (!peer || !pinned || X509_cmp(peer.get(), pinned.get()) != 0) {
        return std::nullopt;
      }
    }
    if (SSL_write(ssl.get(), request.data(), static_cast<int>(request.size())) <= 0) {
      return std::nullopt;
    }
    int n;
    while ((n = SSL_read(ssl.get(), buffer, sizeof(buffer))) > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
    }
    return body_of(response);
  }

  std::optional<std::string> xml_value(std::string_view xml, std::string_view tag) {
    const auto open = "<" + std::string(tag) + ">";
    const auto close = "</" + std::string(tag) + ">";
    const auto begin = xml.find(open);
    if (begin == std::string_view::npos) {
      return std::nullopt;
    }
    const auto end = xml.find(close, begin + open.size());
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return std::string(xml.substr(begin + open.size(), end - begin - open.size()));
  }

  int xml_status(std::string_view xml) {
    const auto at = xml.find("status_code=\"");
    if (at == std::string_view::npos) {
      return 200;
    }
    return std::atoi(std::string(xml.substr(at + 13, 4)).c_str());
  }

  std::string to_hex(std::string_view bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto c : bytes) {
      out.push_back(digits[static_cast<unsigned char>(c) >> 4]);
      out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
    }
    return out;
  }

  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
  }

}  // namespace mstream
