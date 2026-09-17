// SPDX-License-Identifier: MIT
#include "codecs.h"
#include <cstring>
#include <zstd.h>

namespace pto { namespace detail {
namespace {

#ifndef PTOLIB_ZSTD_DECODE_ONLY
bool zstd_compress(const unsigned char* in, std::size_t n, int level,
                   std::vector<unsigned char>& out) {
    const std::size_t bound = ZSTD_compressBound(n);
    if (ZSTD_isError(bound)) return false;
    out.resize(bound);
    const std::size_t got = ZSTD_compress(out.data(), out.size(), in, n,
                                          level < 0 ? ZSTD_CLEVEL_DEFAULT : level);
    if (ZSTD_isError(got)) { out.clear(); return false; }
    out.resize(got);
    return true;
}
#endif
bool zstd_decompress(const unsigned char* in, std::size_t n, std::size_t raw_size,
                     std::vector<unsigned char>& out) {
    if (raw_size == 0) {
        const unsigned long long known = ZSTD_getFrameContentSize(in, n);
        if (known != ZSTD_CONTENTSIZE_UNKNOWN && known != ZSTD_CONTENTSIZE_ERROR)
            raw_size = static_cast<std::size_t>(known);
    }
    if (raw_size != 0) {
        out.resize(raw_size);
        const std::size_t got = ZSTD_decompress(out.data(), out.size(), in, n);
        if (ZSTD_isError(got) || got != raw_size) { out.clear(); return false; }
        return true;
    }
    // Size unknown: stream it out, growing as it comes.
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (ds == nullptr) return false;
    if (ZSTD_isError(ZSTD_initDStream(ds))) {
        ZSTD_freeDStream(ds);
        out.clear();
        return false;
    }
    ZSTD_inBuffer src = {in, n, 0};
    out.clear();
    std::vector<unsigned char> chunk(ZSTD_DStreamOutSize());
    bool ok = false;
    for (;;) {
        const std::size_t previous_pos = src.pos;
        ZSTD_outBuffer dst = {chunk.data(), chunk.size(), 0};
        const std::size_t r = ZSTD_decompressStream(ds, &dst, &src);
        if (ZSTD_isError(r)) break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(dst.pos));
        if (r == 0) { ok = true; break; }
        // Only the decoder can confirm frame completion. Allow buffered
        // output to drain after input ends, but reject an incomplete frame
        // as soon as the decoder cannot make further progress.
        if (src.pos == previous_pos && dst.pos == 0) break;
    }
    ZSTD_freeDStream(ds);
    if (!ok) out.clear();
    return ok;
}
}  // namespace

Codec zstd_codec() {
    Codec codec;
    codec.name = "zstd";
#ifndef PTOLIB_ZSTD_DECODE_ONLY
    codec.compress = &zstd_compress;
#endif
    codec.decompress = &zstd_decompress;
    return codec;
}
}}  // namespace pto::detail
