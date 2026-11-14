/**
 * @file gzip_util.h
 * @brief Minimal gzip (RFC 1952) decoder; ROM miniz on device, zlib on host
 */

#ifndef GZIP_UTIL_H
#define GZIP_UTIL_H

#include <cstddef>
#include <string>

namespace ink {

/** @brief True when data starts with the gzip magic bytes */
bool IsGzip(const std::string& data);

/**
 * @brief Inflate a complete gzip member into out
 * @param max_out Upper bound on the decompressed size
 * @return false on a malformed stream or when the output exceeds max_out
 */
bool Gunzip(const std::string& in, size_t max_out, std::string* out);

}  // namespace ink

#endif  // GZIP_UTIL_H
