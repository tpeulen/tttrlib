# Vendored brotli (google/brotli, MIT)

`.drot` rotamer libraries (PRD-118) are brotli-compressed tar streams, so
IMP.bff both **reads** and **writes** brotli. The codec is vendored here so
the module declares no brotli dependency: `dependencies.py` is empty of it,
no `-lbrotlidec`/`-lbrotlienc` reaches the link line, and the headers used
are the ones in `include/brotli/` -- never a brotli that happens to sit in
the build environment.

Upstream: <https://github.com/google/brotli> at master commit
`8e10eeb3378f6c459dbaf033ca6727e9816afccb` (2026-08-04; `version.h` says
1.2.0). Vendored: `c/common`, `c/dec`, `c/enc`, `c/include/brotli`, `LICENSE`.
Not vendored: `c/tools` (the CLI) and `enc/static_init_lazy.cc` (an optional
C++ initialiser the amalgamation does not use).

## What was changed against upstream

Mechanical only -- no numerics, no logic:

1. `common/*.c`, `dec/*.c`, `enc/*.c` → `*.inc`. The suffix keeps IMP's
   source walkers off them: an IMP module is compiled from the `.cpp` files
   sitting directly in `src/`, and a `.cpp` here would be compiled a second
   time in IMP's per-file mode, defining every brotli symbol twice.
   `src/Brotli.cpp` includes all of them, so the codec is one translation
   unit, compiled with hidden visibility (nothing named `Brotli*` is
   exported from `libimp_bff`).
2. `#include <brotli/x.h>` → the relative path to the header in this tree
   (`"../include/brotli/x.h"`, or `"x.h"` inside `include/brotli/`). No
   include directory has to be added to the build, and no other brotli can
   be picked up by accident.
3. Two consequences of the single translation unit, both handled in
   `src/Brotli.cpp` rather than in the vendored source: the second copy of
   three file-local helpers (`BrotliStoreMetaBlockHeader`,
   `RewindBitPosition`, `SortHuffmanTree`) and of `MIN_RATIO` is renamed
   around its `#include`.
4. One edit inside a vendored file, marked `[imp.bff]` in
   `dec/decode.inc`: an explicit cast to `BrotliDecoderErrorCode` in the
   `BROTLI_SAFE` macro, because C++ does not convert `int` to an enum
   implicitly the way C does.

The sources otherwise compile as C++ unchanged; upstream supports that
(`extern "C"` guards and cast-clean allocation macros are already there).

## Refreshing from upstream

```sh
u=/path/to/brotli                       # a github.com/google/brotli checkout
d=src/brotli
rm -rf $d/common $d/dec $d/enc $d/include
mkdir -p $d/include
cp -R $u/c/common $u/c/dec $u/c/enc $d/
cp -R $u/c/include/brotli $d/include/
cp $u/LICENSE $d/
rm -f $d/enc/static_init_lazy.cc
for f in $d/common/*.c $d/dec/*.c $d/enc/*.c; do mv "$f" "${f%.c}.inc"; done
sed -i '' -E 's|#include <brotli/([A-Za-z_]+\.h)>|#include "\1"|' \
    $d/include/brotli/*.h
sed -i '' -E 's|#include <brotli/([A-Za-z_]+\.h)>|#include "../include/brotli/\1"|' \
    $d/common/* $d/dec/* $d/enc/*
```

Then re-apply edit 4 (the compiler points at it), check the include list in
`src/Brotli.cpp` still matches the `.inc` files present, and rebuild.

## Verification

- The decoder is byte-identical to the reference `brotli` CLI on the `.drot`
  corpus (same SHA-1 of the decoded tar), and `IMP.bff.read_drot`
  reconstructs those libraries through it.
- The encoder at quality 11 / window 24 produces **byte-identical output**
  to the reference `brotli` python module on both a rotamer library and a
  source file, and its streams decode with that module.
- `test/io/test_drot.py` runs a write → read round trip through the module.
