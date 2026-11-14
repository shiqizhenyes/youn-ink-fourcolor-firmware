/**
 * @file qweather_jwt.cc
 * @brief QWeather JWT (EdDSA / Ed25519) and URL helpers, libsodium only
 */

#include "qweather_jwt.h"

#include <sodium.h>

namespace ink {

namespace {

constexpr int kIatSkewSeconds = 30;
constexpr int kLifetimeSeconds = 900;

bool EnsureSodium() {
    static const bool ready = sodium_init() >= 0;
    return ready;
}

}  // namespace

std::string UrlEncode(const std::string& in) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

std::string Base64UrlEncode(const std::string& in) {
    constexpr int kVariant = sodium_base64_VARIANT_URLSAFE_NO_PADDING;
    std::string out(sodium_base64_encoded_len(in.size(), kVariant), '\0');
    sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(in.data()),
                      in.size(), kVariant);
    out.resize(out.find('\0'));
    return out;
}

std::string Ed25519SignBase64Url(const std::string& seed, const std::string& message) {
    if (seed.size() != crypto_sign_SEEDBYTES || !EnsureSodium()) return "";
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    if (crypto_sign_seed_keypair(pk, sk, reinterpret_cast<const unsigned char*>(seed.data())) != 0) {
        return "";
    }
    unsigned char sig[crypto_sign_BYTES];
    const int rc = crypto_sign_detached(sig, nullptr,
                                        reinterpret_cast<const unsigned char*>(message.data()),
                                        message.size(), sk);
    sodium_memzero(sk, sizeof(sk));
    if (rc != 0) return "";
    return Base64UrlEncode(std::string(reinterpret_cast<const char*>(sig), sizeof(sig)));
}

std::string BuildQWeatherJwt(const std::string& kid, const std::string& sub,
                             const std::string& iss, const std::string& seed_b64,
                             int64_t now) {
    if (kid.empty() || sub.empty() || iss.empty() || seed_b64.empty() || !EnsureSodium()) {
        return "";
    }
    std::string seed(crypto_sign_SEEDBYTES, '\0');
    size_t seed_len = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(seed.data()), seed.size(),
                          seed_b64.data(), seed_b64.size(), " \r\n", &seed_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        seed_len != crypto_sign_SEEDBYTES) {
        return "";
    }

    const int64_t iat = now - kIatSkewSeconds;
    const std::string header = "{\"alg\":\"EdDSA\",\"kid\":\"" + kid + "\"}";
    const std::string payload = "{\"iss\":\"" + iss + "\",\"sub\":\"" + sub +
                                "\",\"iat\":" + std::to_string(iat) +
                                ",\"exp\":" + std::to_string(now + kLifetimeSeconds) + "}";
    const std::string signing_input = Base64UrlEncode(header) + "." + Base64UrlEncode(payload);
    const std::string sig = Ed25519SignBase64Url(seed, signing_input);
    sodium_memzero(seed.data(), seed.size());
    if (sig.empty()) return "";
    return signing_input + "." + sig;
}

}  // namespace ink
