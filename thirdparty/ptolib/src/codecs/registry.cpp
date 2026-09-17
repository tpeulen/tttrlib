// SPDX-License-Identifier: MIT
#include "codecs.h"

namespace pto { namespace detail {
std::vector<Codec> builtin_codecs() {
    std::vector<Codec> result;
#ifdef PTOLIB_ENABLE_ZSTD
    result.push_back(zstd_codec());
#endif
#ifdef PTOLIB_ENABLE_BROTLI
    result.push_back(brotli_codec());
#endif
#ifdef PTOLIB_ENABLE_LZ4
    result.push_back(lz4_codec());
#endif
#ifdef PTOLIB_ENABLE_DEFLATE
    result.push_back(deflate_codec());
#endif
    return result;
}
}}  // namespace pto::detail
