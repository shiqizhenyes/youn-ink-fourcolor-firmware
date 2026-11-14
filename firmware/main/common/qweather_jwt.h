/**
 * @file qweather_jwt.h
 * @brief QWeather JWT (EdDSA / Ed25519) and URL helpers, libsodium only
 *
 * Token: base64url(header).base64url(payload).base64url(Ed25519 signature),
 * header {"alg":"EdDSA","kid":..}, payload {"iss","sub","iat","exp"}.
 */

#ifndef QWEATHER_JWT_H
#define QWEATHER_JWT_H

#include <cstdint>
#include <string>

namespace ink {

std::string UrlEncode(const std::string& in);

/** @brief Base64URL without padding (RFC 7515) */
std::string Base64UrlEncode(const std::string& in);

/**
 * @brief Ed25519 signature of message with the 32-byte seed
 * @return Base64URL signature, or empty on failure
 */
std::string Ed25519SignBase64Url(const std::string& seed, const std::string& message);

/**
 * @brief Signed JWT valid from now-30 s to now+900 s
 * @param seed_b64 Base64 (standard alphabet) of the 32-byte Ed25519 seed
 * @return token, or empty on failure
 */
std::string BuildQWeatherJwt(const std::string& kid, const std::string& sub,
                             const std::string& iss, const std::string& seed_b64,
                             int64_t now);

}  // namespace ink

#endif  // QWEATHER_JWT_H
