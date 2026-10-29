/**
 * @file tools/mstream-client/src/pairing.cpp
 * @brief GameStream pairing (PIN): the same steps as Moonlight's NvPairingManager (Gen 7+, SHA-256).
 */
// standard includes
#include <memory>

// lib includes
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

// local includes
#include "pairing.h"

namespace mstream {

  namespace {
    constexpr int http_port = 47989;
    constexpr int https_port = 47984;

    std::string random_bytes(std::size_t n) {
      std::string out(n, '\0');
      RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), static_cast<int>(n));
      return out;
    }

    std::string sha256(const std::string &data) {
      std::string out(SHA256_DIGEST_LENGTH, '\0');
      SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), reinterpret_cast<unsigned char *>(out.data()));
      return out;
    }

    std::string aes_ecb(const std::string &data, const std::string &key, bool encrypt) {
      std::string out(data.size(), '\0');
      auto *ctx = EVP_CIPHER_CTX_new();
      int length = 0;
      if (encrypt) {
        EVP_EncryptInit(ctx, EVP_aes_128_ecb(), reinterpret_cast<const unsigned char *>(key.data()), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length, reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()));
      } else {
        EVP_DecryptInit(ctx, EVP_aes_128_ecb(), reinterpret_cast<const unsigned char *>(key.data()), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length, reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()));
      }
      EVP_CIPHER_CTX_free(ctx);
      return out;
    }

    X509 *read_cert(const std::string &pem) {
      auto *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
      auto *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);
      return cert;
    }

    std::string signature_of(const std::string &pem) {
      auto *cert = read_cert(pem);
      if (!cert) {
        return {};
      }
      const ASN1_BIT_STRING *signature = nullptr;
      X509_get0_signature(&signature, nullptr, cert);
      std::string out(reinterpret_cast<const char *>(ASN1_STRING_get0_data(signature)), static_cast<std::size_t>(ASN1_STRING_length(signature)));
      X509_free(cert);
      return out;
    }

    bool verify(const std::string &data, const std::string &signature, const std::string &cert_pem) {
      auto *cert = read_cert(cert_pem);
      if (!cert) {
        return false;
      }
      auto *key = X509_get_pubkey(cert);
      auto *ctx = EVP_MD_CTX_new();
      EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key);
      EVP_DigestVerifyUpdate(ctx, data.data(), data.size());
      const int ok = EVP_DigestVerifyFinal(ctx, reinterpret_cast<const unsigned char *>(signature.data()), signature.size());
      EVP_MD_CTX_free(ctx);
      EVP_PKEY_free(key);
      X509_free(cert);
      return ok == 1;
    }

    std::string sign(const std::string &data, const std::string &key_pem) {
      auto *bio = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
      auto *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);
      auto *ctx = EVP_MD_CTX_new();
      std::size_t length = 0;
      EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key);
      EVP_DigestSignUpdate(ctx, data.data(), data.size());
      EVP_DigestSignFinal(ctx, nullptr, &length);
      std::string out(length, '\0');
      EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char *>(out.data()), &length);
      out.resize(length);
      EVP_MD_CTX_free(ctx);
      EVP_PKEY_free(key);
      return out;
    }
  }  // namespace

  std::string pair(const std::string &host, const std::string &identity_dir, identity_t &identity, const std::string &pin, const std::string &device_name) {
    const auto query = [&](const std::string &rest) {
      return "/pair?uniqueid=" + identity.unique_id + "&devicename=" + device_name + "&updateState=1&" + rest;
    };
    const auto step = [&](int number, const std::string &rest, bool tls, int timeout_ms) -> std::optional<std::string> {
      auto reply = http_get(host, tls ? https_port : http_port, tls, query(rest), identity, timeout_ms);
      if (!reply || xml_status(*reply) != 200 || xml_value(*reply, "paired") != "1") {
        http_get(host, http_port, false, "/unpair?uniqueid=" + identity.unique_id, identity, 5000);
        return std::nullopt;
      }
      (void) number;
      return reply;
    };

    // Stage 1: the host waits for the PIN, then sends its certificate
    const auto salt = random_bytes(16);
    auto aes_key = sha256(salt + pin);
    aes_key.resize(16);
    auto cert_reply = step(1, "phrase=getservercert&salt=" + to_hex(salt) + "&clientcert=" + to_hex(identity.cert_pem), false, 180000);
    if (!cert_reply) {
      return "stage 1 (getservercert) failed";
    }
    const auto server_cert = from_hex(xml_value(*cert_reply, "plaincert").value_or(""));
    if (server_cert.empty()) {
      return "no server certificate (pairing already in progress?)";
    }

    // Stage 2: our challenge
    const auto challenge = random_bytes(16);
    auto challenge_reply = step(2, "clientchallenge=" + to_hex(aes_ecb(challenge, aes_key, true)), false, 10000);
    if (!challenge_reply) {
      return "stage 2 (clientchallenge) failed";
    }
    const auto response = aes_ecb(from_hex(xml_value(*challenge_reply, "challengeresponse").value_or("")), aes_key, false);
    if (response.size() < 48) {
      return "invalid challengeresponse";
    }
    const auto server_response = response.substr(0, 32);
    const auto server_challenge = response.substr(32, 16);

    // Stage 3: answer the host's challenge
    const auto client_secret = random_bytes(16);
    auto hash = sha256(server_challenge + signature_of(identity.cert_pem) + client_secret);
    hash.resize(32);
    auto secret_reply = step(3, "serverchallengeresp=" + to_hex(aes_ecb(hash, aes_key, true)), false, 10000);
    if (!secret_reply) {
      return "stage 3 (serverchallengeresp) failed";
    }
    const auto pairing_secret = from_hex(xml_value(*secret_reply, "pairingsecret").value_or(""));
    if (pairing_secret.size() <= 16) {
      return "invalid pairingsecret";
    }
    const auto server_secret = pairing_secret.substr(0, 16);
    if (!verify(server_secret, pairing_secret.substr(16), server_cert)) {
      return "server signature doesn't verify";
    }
    if (sha256(challenge + signature_of(server_cert) + server_secret) != server_response) {
      return "wrong PIN";
    }

    // Stage 4: our signed secret
    if (!step(4, "clientpairingsecret=" + to_hex(client_secret + sign(client_secret, identity.key_pem)), false, 10000)) {
      return "stage 4 (clientpairingsecret) failed";
    }

    // Stage 5: over HTTPS with our certificate, pinning the host's
    identity.server_cert_pem = server_cert;
    if (!step(5, "phrase=pairchallenge", true, 10000)) {
      identity.server_cert_pem.clear();
      return "stage 5 (pairchallenge over HTTPS) failed";
    }
    if (!save_server_cert(identity_dir, server_cert)) {
      return "couldn't save the server certificate";
    }
    return {};
  }

}  // namespace mstream
