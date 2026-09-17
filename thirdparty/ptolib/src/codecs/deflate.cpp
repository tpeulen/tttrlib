// SPDX-License-Identifier: MIT
#include "codecs.h"
#include <cstring>
#ifdef PTOLIB_WITH_DEFLATE
#include <zlib.h>
#else
#include <miniz.h>
#endif

namespace pto { namespace detail {
namespace {

#ifndef PTOLIB_DEFLATE_DECODE_ONLY
bool deflate_compress(const unsigned char* in, std::size_t n, int level,
                      std::vector<unsigned char>& out) {
#if defined(PTOLIB_WITH_DEFLATE)
    uLongf bound = compressBound(static_cast<uLong>(n));
    out.resize(bound);
    uLongf got = bound;
    if (compress2(out.data(), &got, in, static_cast<uLong>(n),
                  level < 0 ? Z_DEFAULT_COMPRESSION : level) != Z_OK) {
        out.clear();
        return false;
    }
#else
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(n));
    out.resize(bound);
    mz_ulong got = bound;
    if (mz_compress2(out.data(), &got, in, static_cast<mz_ulong>(n),
                     level < 0 ? MZ_DEFAULT_LEVEL : level) != MZ_OK) {
        out.clear();
        return false;
    }
#endif
    out.resize(static_cast<std::size_t>(got));
    return true;
}
#endif
bool deflate_decompress(const unsigned char* in, std::size_t n, std::size_t raw_size,
                        std::vector<unsigned char>& out) {
    if (raw_size != 0) {
        out.resize(raw_size);
#if defined(PTOLIB_WITH_DEFLATE)
        uLongf got = raw_size;
        if (uncompress(out.data(), &got, in, static_cast<uLong>(n)) != Z_OK ||
            got != raw_size) {
#else
        mz_ulong got = raw_size;
        if (mz_uncompress(out.data(), &got, in, static_cast<mz_ulong>(n)) != MZ_OK ||
            got != raw_size) {
#endif
            out.clear();
            return false;
        }
        return true;
    }
    // Size unknown: inflate and grow as it comes.
    std::vector<unsigned char> chunk(1u << 16);
    bool ok = true;
    out.clear();
#if defined(PTOLIB_WITH_DEFLATE)
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (inflateInit(&s) != Z_OK) return false;
    s.next_in = const_cast<Bytef*>(in);
    s.avail_in = static_cast<uInt>(n);
    for (;;) {
        s.next_out = chunk.data();
        s.avail_out = static_cast<uInt>(chunk.size());
        const int r = inflate(&s, Z_NO_FLUSH);
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(chunk.size() - s.avail_out));
        if (r == Z_STREAM_END) break;
        if (r != Z_OK) { ok = false; break; }
    }
    inflateEnd(&s);
#else
    std::size_t got = 0;
    void* raw = tinfl_decompress_mem_to_heap(in, n, &got, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (raw == nullptr) return false;
    out.assign(static_cast<unsigned char*>(raw),
               static_cast<unsigned char*>(raw) + got);
    mz_free(raw);
#endif
    if (!ok) out.clear();
    return ok;
}
}  // namespace

Codec deflate_codec() {
    Codec codec;
    codec.name = "deflate";
#ifndef PTOLIB_DEFLATE_DECODE_ONLY
    codec.compress = &deflate_compress;
#endif
    codec.decompress = &deflate_decompress;
    return codec;
}
}}  // namespace pto::detail
