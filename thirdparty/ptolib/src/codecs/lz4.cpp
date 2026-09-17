// SPDX-License-Identifier: MIT
#include "codecs.h"
#include <cstring>
#include <lz4frame.h>

namespace pto { namespace detail {
namespace {

#ifndef PTOLIB_LZ4_DECODE_ONLY
bool lz4_compress(const unsigned char* in, std::size_t n, int level,
                  std::vector<unsigned char>& out) {
    LZ4F_preferences_t prefs;
    std::memset(&prefs, 0, sizeof(prefs));
    prefs.frameInfo.contentSize = n;
    prefs.frameInfo.blockSizeID = LZ4F_max4MB;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    prefs.compressionLevel = level < 0 ? 0 : level;   // 0 fast; 3+ the HC coder, up to 12
    const std::size_t bound = LZ4F_compressFrameBound(n, &prefs);
    out.resize(bound);
    const std::size_t got = LZ4F_compressFrame(out.data(), bound, in, n, &prefs);
    if (LZ4F_isError(got)) { out.clear(); return false; }
    out.resize(got);
    return true;
}
#endif
bool lz4_decompress(const unsigned char* in, std::size_t n, std::size_t raw_size,
                    std::vector<unsigned char>& out) {
    LZ4F_dctx* d = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&d, LZ4F_VERSION))) return false;
    out.clear();
    if (raw_size == 0) {
        LZ4F_frameInfo_t info;
        std::size_t peek = n;
        const std::size_t r = LZ4F_getFrameInfo(d, &info, in, &peek);
        if (!LZ4F_isError(r) && info.contentSize != 0) raw_size = static_cast<std::size_t>(info.contentSize);
        LZ4F_resetDecompressionContext(d);
    }
    out.reserve(raw_size);
    std::vector<unsigned char> chunk(1u << 20);
    std::size_t consumed = 0;
    bool ok = true;
    for (;;) {
        std::size_t dst_size = chunk.size();
        std::size_t src_size = n - consumed;
        const std::size_t r = LZ4F_decompress(d, chunk.data(), &dst_size, in + consumed, &src_size, nullptr);
        if (LZ4F_isError(r)) { ok = false; break; }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(dst_size));
        consumed += src_size;
        if (r == 0) break;                       // frame complete
        if (src_size == 0 && dst_size == 0) { ok = false; break; }   // truncated
    }
    LZ4F_freeDecompressionContext(d);
    if (ok && raw_size != 0 && out.size() != raw_size) ok = false;
    if (!ok) out.clear();
    return ok;
}
}  // namespace

Codec lz4_codec() {
    Codec codec;
    codec.name = "lz4";
#ifndef PTOLIB_LZ4_DECODE_ONLY
    codec.compress = &lz4_compress;
#endif
    codec.decompress = &lz4_decompress;
    return codec;
}
}}  // namespace pto::detail
