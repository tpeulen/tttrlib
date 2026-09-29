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
        // One context per thread: a fresh one per call is an allocation of
        // ~100 kB, which dominates when a reader decodes many small segments.
        struct Context {
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            ~Context() { ZSTD_freeDCtx(dctx); }
        };
        thread_local Context context;
        const std::size_t got = context.dctx != nullptr
                ? ZSTD_decompressDCtx(context.dctx, out.data(), out.size(), in, n)
                : ZSTD_decompress(out.data(), out.size(), in, n);
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

#ifndef PTOLIB_ZSTD_DECODE_ONLY
}  // namespace
}}  // namespace pto::detail
extern "C" {
// zdict.h's two entry points; the amalgamation and libzstd both carry them.
size_t ZDICT_trainFromBuffer(void* dictBuffer, size_t dictBufferCapacity, const void* samplesBuffer,
                             const size_t* samplesSizes, unsigned nbSamples);
unsigned ZDICT_isError(size_t code);
}
namespace pto { namespace detail {
namespace {
bool zstd_train(const unsigned char* samples, const std::size_t* sizes, std::size_t count,
                std::size_t capacity, std::vector<unsigned char>& dictionary) {
    dictionary.resize(capacity);
    const std::size_t got = ZDICT_trainFromBuffer(dictionary.data(), capacity, samples, sizes,
                                                  static_cast<unsigned>(count));
    if (ZDICT_isError(got)) { dictionary.clear(); return false; }
    dictionary.resize(got);
    return true;
}

bool zstd_compress_with(const unsigned char* in, std::size_t n, int level,
                        const std::vector<unsigned char>& dictionary,
                        std::vector<unsigned char>& out) {
    struct Context {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ~Context() { ZSTD_freeCCtx(cctx); }
    };
    thread_local Context context;
    out.resize(ZSTD_compressBound(n));
    const std::size_t got = ZSTD_compress_usingDict(context.cctx, out.data(), out.size(), in, n,
                                                    dictionary.data(), dictionary.size(),
                                                    level < 0 ? ZSTD_CLEVEL_DEFAULT : level);
    if (ZSTD_isError(got)) { out.clear(); return false; }
    out.resize(got);
    return true;
}
#endif

bool zstd_decompress_with(const unsigned char* in, std::size_t n, std::size_t raw_size,
                          const std::vector<unsigned char>& dictionary,
                          std::vector<unsigned char>& out) {
    // the digested dictionary, per thread, for the last dictionary seen
    struct Digest {
        const void* key = nullptr;
        std::size_t size = 0;
        ZSTD_DDict* ddict = nullptr;
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        ~Digest() { if (ddict) ZSTD_freeDDict(ddict); ZSTD_freeDCtx(dctx); }
    };
    thread_local Digest d;
    if (d.key != dictionary.data() || d.size != dictionary.size() || d.ddict == nullptr) {
        if (d.ddict) ZSTD_freeDDict(d.ddict);
        d.ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
        d.key = dictionary.data();
        d.size = dictionary.size();
        if (d.ddict == nullptr) return false;
    }
    if (raw_size == 0) {
        const unsigned long long known = ZSTD_getFrameContentSize(in, n);
        if (known == ZSTD_CONTENTSIZE_UNKNOWN || known == ZSTD_CONTENTSIZE_ERROR) return false;
        raw_size = static_cast<std::size_t>(known);
    }
    out.resize(raw_size);
    const std::size_t got = ZSTD_decompress_usingDDict(d.dctx, out.data(), out.size(), in, n, d.ddict);
    if (ZSTD_isError(got) || got != raw_size) { out.clear(); return false; }
    return true;
}
}  // namespace

Codec zstd_codec() {
    Codec codec;
    codec.name = "zstd";
#ifndef PTOLIB_ZSTD_DECODE_ONLY
    codec.compress = &zstd_compress;
    codec.train = &zstd_train;
    codec.compress_with = &zstd_compress_with;
#endif
    codec.decompress = &zstd_decompress;
    codec.decompress_with = &zstd_decompress_with;
    return codec;
}
}}  // namespace pto::detail
