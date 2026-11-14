/**
 * @file gzip_util.cc
 * @brief Minimal gzip (RFC 1952) decoder; ROM miniz on device, zlib on host
 */

#include "gzip_util.h"

#include <cstdint>
#include <cstdlib>

#ifdef ESP_PLATFORM
#include <miniz.h>
#else
#include <zlib.h>
#endif

namespace ink {

namespace {

constexpr uint8_t kFlagHcrc = 0x02;
constexpr uint8_t kFlagExtra = 0x04;
constexpr uint8_t kFlagName = 0x08;
constexpr uint8_t kFlagComment = 0x10;
constexpr size_t kHeaderSize = 10;
constexpr size_t kTrailerSize = 8;

/** @return offset of the raw deflate data, or 0 on a malformed header */
size_t DeflateOffset(const uint8_t* p, size_t n) {
    if (n < kHeaderSize + kTrailerSize || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return 0;
    const uint8_t flags = p[3];
    size_t pos = kHeaderSize;
    if (flags & kFlagExtra) {
        if (pos + 2 > n) return 0;
        pos += 2 + (p[pos] | (p[pos + 1] << 8));
    }
    for (uint8_t f : {kFlagName, kFlagComment}) {
        if (!(flags & f)) continue;
        while (pos < n && p[pos] != 0) ++pos;
        ++pos;
    }
    if (flags & kFlagHcrc) pos += 2;
    return pos + kTrailerSize <= n ? pos : 0;
}

bool InflateRaw(const uint8_t* in, size_t in_len, size_t max_out, std::string* out) {
#ifdef ESP_PLATFORM
    auto* inflator = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
    if (!inflator) return false;
    tinfl_init(inflator);
    out->assign(max_out, '\0');
    auto* dst = reinterpret_cast<mz_uint8*>(out->data());
    size_t in_size = in_len;
    size_t out_size = max_out;
    const tinfl_status status = tinfl_decompress(inflator, in, &in_size, dst, dst, &out_size,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(inflator);
    if (status != TINFL_STATUS_DONE) return false;
    out->resize(out_size);
    return true;
#else
    z_stream zs = {};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    out->assign(max_out, '\0');
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = static_cast<uInt>(in_len);
    zs.next_out = reinterpret_cast<Bytef*>(out->data());
    zs.avail_out = static_cast<uInt>(max_out);
    const int rc = inflate(&zs, Z_FINISH);
    const size_t produced = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return false;
    out->resize(produced);
    return true;
#endif
}

}  // namespace

bool IsGzip(const std::string& data) {
    return data.size() >= 2 && static_cast<uint8_t>(data[0]) == 0x1f &&
           static_cast<uint8_t>(data[1]) == 0x8b;
}

bool Gunzip(const std::string& in, size_t max_out, std::string* out) {
    const auto* p = reinterpret_cast<const uint8_t*>(in.data());
    const size_t offset = DeflateOffset(p, in.size());
    if (offset == 0) return false;
    if (!InflateRaw(p + offset, in.size() - offset - kTrailerSize, max_out, out)) return false;
    const uint8_t* trailer = p + in.size() - 4;
    const uint32_t isize = trailer[0] | (trailer[1] << 8) | (trailer[2] << 16) |
                           (static_cast<uint32_t>(trailer[3]) << 24);
    return isize == static_cast<uint32_t>(out->size());
}

}  // namespace ink
