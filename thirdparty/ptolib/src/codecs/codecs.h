// SPDX-License-Identifier: MIT
#ifndef PTOLIB_DETAIL_CODECS_H
#define PTOLIB_DETAIL_CODECS_H
#include <ptolib/ptolib.h>
// Internal to one ptolib build: never exported, so a library that carries
// ptolib cannot lend another one its (maybe older) codec table.
#if defined(__GNUC__) && !defined(_WIN32)
#define PTOLIB_INTERNAL __attribute__((visibility("hidden")))
#else
#define PTOLIB_INTERNAL
#endif
namespace pto { namespace detail {
PTOLIB_INTERNAL std::vector<Codec> builtin_codecs();
PTOLIB_INTERNAL Codec zstd_codec();
PTOLIB_INTERNAL Codec brotli_codec();
PTOLIB_INTERNAL Codec lz4_codec();
PTOLIB_INTERNAL Codec deflate_codec();
}}  // namespace pto::detail
#endif
