// SPDX-License-Identifier: MIT
#include "codecs.h"
#include <cstring>
#include <brotli/decode.h>
#ifndef PTOLIB_BROTLI_DECODE_ONLY
#include <brotli/encode.h>
#endif

namespace pto { namespace detail {
namespace {

#ifndef PTOLIB_BROTLI_DECODE_ONLY
bool brotli_compress(const unsigned char* in, std::size_t n, int level,
                     std::vector<unsigned char>& out) {
    // Quality 5 by default, not brotli's own 11: on numeric columns 11 is
    // twenty times slower than 5 for no more ratio, and a caller that wants
    // an archive setting asks for it. The window is the largest standard one
    // (24) at every quality, so a stream written here at quality 11 is the
    // byte-identical twin of the reference tool's `brotli -q 11 -w 24` --
    // what the .drot libraries are pinned against.
    int quality = level < 0 ? 5 : level;
    if (quality > BROTLI_MAX_QUALITY) quality = BROTLI_MAX_QUALITY;
    if (quality < BROTLI_MIN_QUALITY) quality = BROTLI_MIN_QUALITY;
    std::size_t size = BrotliEncoderMaxCompressedSize(n);
    if (size == 0) size = n + 64;
    out.resize(size);
    if (!BrotliEncoderCompress(quality, BROTLI_MAX_WINDOW_BITS, BROTLI_MODE_GENERIC, n, in,
                               &size, out.data())) {
        out.clear();
        return false;
    }
    out.resize(size);
    return true;
}
#endif
bool brotli_decompress(const unsigned char* in, std::size_t n, std::size_t raw_size,
                       std::vector<unsigned char>& out) {
    if (raw_size != 0) {
        out.resize(raw_size);
        std::size_t size = raw_size;
        if (BrotliDecoderDecompress(n, in, &size, out.data()) != BROTLI_DECODER_RESULT_SUCCESS ||
            size != raw_size) {
            out.clear();
            return false;
        }
        return true;
    }
    // Size unknown -- a stream from a writer that recorded none: stream it out.
    BrotliDecoderState* st = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (st == nullptr) return false;
    std::size_t available_in = n;
    const std::uint8_t* next_in = in;
    out.clear();
    std::vector<unsigned char> chunk(1u << 16);
    bool ok = true;
    for (;;) {
        std::size_t available_out = chunk.size();
        std::uint8_t* next_out = chunk.data();
        const BrotliDecoderResult r = BrotliDecoderDecompressStream(
                st, &available_in, &next_in, &available_out, &next_out, nullptr);
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(chunk.size() - available_out));
        if (r == BROTLI_DECODER_RESULT_SUCCESS) break;
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) continue;
        ok = false;   // needs more input (truncated) or an error
        break;
    }
    BrotliDecoderDestroyInstance(st);
    if (!ok) out.clear();
    return ok;
}
}  // namespace

Codec brotli_codec() {
    Codec codec;
    codec.name = "brotli";
#ifndef PTOLIB_BROTLI_DECODE_ONLY
    codec.compress = &brotli_compress;
#endif
    codec.decompress = &brotli_decompress;
    return codec;
}
}}  // namespace pto::detail
