// SPDX-License-Identifier: MIT
#ifndef PTOLIB_DETAIL_CODECS_H
#define PTOLIB_DETAIL_CODECS_H
#include <ptolib/ptolib.h>
namespace pto { namespace detail {
std::vector<Codec> builtin_codecs();
Codec zstd_codec();
Codec brotli_codec();
Codec lz4_codec();
Codec deflate_codec();
}}  // namespace pto::detail
#endif
