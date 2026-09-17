# ptolib

[![CI](https://github.com/tpeulen/ptolib/actions/workflows/ci.yml/badge.svg)](https://github.com/tpeulen/ptolib/actions/workflows/ci.yml)

A C++14 library for self-contained, packed data streams. A small public header
exposes the container and table APIs; the implementation and optional codecs
compile separately. Bundled codecs and nlohmann/json are included in the source
package, so the default build needs no dependency downloads:

- **PTO, Portable Tagged Objects** -- an EBML document (`DocType "pto"`) that
  binds opaque payloads into one file, gives each a UID that survives
  rewriting, lets typed tags describe them and reference each other, and
  updates in place: recomputing a table beside an 8 GiB stream rewrites the
  table, not the file. Two indexes with generation and CRC support selection
  of an intact index. In-place edits are not transactions; see the persistence
  guarantees below. A `.pto` can carry an executable stub and open itself.
- **DataStore and `.dstore`** -- a columnar table (typed columns, dictionary
  strings, bit-packed masks, attributes, groups, a compiled SIMD expression
  engine for gating) and its file encoding, which is also the natural payload
  of a PTO object.

Why it exists, and why the framing is EBML: [`docs/motivation.md`](docs/motivation.md).
Specifications: [`docs/pto.rst`](docs/pto.rst) (the container, normative),
[`docs/dstore.md`](docs/dstore.md) (the table file),
[`docs/pto-binary-decoding.md`](docs/pto-binary-decoding.md) (a self-contained
C99 decoding guide).

## Using it

Include the public header and link the compiled `ptolib::ptolib` target:

```cmake
add_subdirectory(thirdparty/ptolib)
target_link_libraries(my_application PRIVATE ptolib::ptolib)
```

```cpp
#include <ptolib/ptolib.h>
```

For an installed package, use `find_package(ptolib CONFIG REQUIRED)` in place
of `add_subdirectory()`. The target supplies the C++14 requirement and codec
link dependencies. JSON headers are private to the library build; applications
that include the public header do not need them. To use an existing JSON copy,
set `PTOLIB_JSON_INCLUDE_DIR` to the directory containing `nlohmann/json.hpp`.

Build in `Release` mode for the expression engine's compiler-vectorised loops.
Do not define `PTOLIB_IMPLEMENTATION` with the maintained source package; that
macro is supported only by the optional generated single-header artifact below.

```cpp
pto::File f;
f.create("run.pto", "a measurement");
std::uint64_t uid = f.add("table", "raw", "notes.txt", bytes, n);

pto::DataStore t("bursts");
t.column(t.add_column("tau", pto::ColumnType::Float64)).set_f64(tau, n_rows);
t.set_n_rows(n_rows);
std::uint64_t tid = pto::pto_add_store(f, "table", "bursts", t);

pto::PtoTag tag;
tag.name = "derived_from"; tag.type = pto::PtoType::UID;
tag.target = tid; tag.u = uid;
f.add_tag(tag);
f.commit();

// later, from any process
pto::File g;
g.open("run.pto");
pto::DataStore back;
pto::pto_read_store(g, g.find("bursts"), back, {"tau"});   // one column: one seek
back.select_expression("tau > 2.5 and tau < 4");           // a bit mask, SIMD
```

A file written by an append-only writer (one SeekHead, all objects in one
`Attachments`) opens read-only and reads like any other; to edit it,
`compact()` it to a new file first.

## Persistence guarantees

`File::commit()` publishes a new index, but `update()` and `remove()` can
modify existing file contents before commit. Closing a writer does not roll
back those operations. Readers must not access a file while it is being
edited. Index checksums detect damaged indexes, not damaged payloads, and
the writer does not guarantee power-loss durability.

For replacement workflows, write and validate a separate file beside the
destination, close it, then atomically replace the destination. Keep the
original until validation succeeds. The standalone `write_store()` follows
a temporary-file replacement workflow; the in-place PTO editing API does
not provide the same isolation.

## Performance

The expression engine evaluates gates a 512-row block at a time straight into
packed bits, in the widest SIMD tier the CPU has: **AVX2** on x86-64 (selected
at run time; the core is compiled for the SSE2 baseline and carries the AVX2
kernels beside it), **NEON** on aarch64, plain vectorised loops elsewhere.
`pto::simd_tier()` reports the tier in use; every tier produces the same bits.
`ptobench` is the scoreboard, and `.benchmarks/` keeps the numbers a change is
judged against:

```sh
./build/ptobench                 # 10 M rows, best of 5
./build/ptobench --simd sse2     # the same machine, one tier down
```

## Compression

The format names four codecs: `zstd`, `brotli`, `lz4`, and `deflate` (a zlib
stream). Their adapters live in `src/codecs/`; bundled upstream C sources live
under `thirdparty/` and compile separately from `src/ptolib.cpp`. Codec source
and dictionary tables are absent from the maintained public header.

Each codec has a CMake provider setting:

| Setting | Default | Accepted values |
|---|---|---|
| `PTOLIB_ZSTD_PROVIDER` | `bundled` | `bundled`, `system`, `disabled` |
| `PTOLIB_BROTLI_PROVIDER` | `bundled` | `bundled`, `system`, `disabled` |
| `PTOLIB_LZ4_PROVIDER` | `bundled` | `bundled`, `system`, `disabled` |
| `PTOLIB_DEFLATE_PROVIDER` | `bundled` | `bundled`, `system`, `disabled` |

`bundled` builds the vendored source without downloading anything. `system`
requires installed headers and libraries and fails configuration if they are
missing; it never downloads a replacement. `disabled` omits the codec and
causes reads of its payloads to fail explicitly. Existing compressed files
keep their encodings: enable their decoders to read them.

Use `-DPTOLIB_DECODE_ONLY=ON` for a reader-only codec build, or
`-DPTOLIB_DECODE_ONLY_CODECS="brotli;deflate"` for selected codecs. Bundled
Brotli omits its encoder translation units, including the large encoder lookup
table; bundled miniz omits its compression APIs. Zstandard and LZ4 have mixed
encoder/decoder source files, so those builds remove compression callbacks but
still compile encoder code. A system Brotli reader links only its common and
decoder libraries; size reductions for other system libraries depend on their
packaging and the linker.

`pto::can_compress(name)` and `pto::can_decompress(name)` report the available
operations. `has_codec(name)` means the codec is registered, which also includes
decoder-only codecs; do not use it to decide whether a writer can compress.
A codec registered with `pto::register_codec` replaces one with the same name.
The legacy CMake `PTOLIB_WITH_*` and `PTOLIB_NO_CODECS` settings remain accepted;
prefer explicit provider settings for new integrations.

Zstandard is the default choice for numeric tables; Brotli remains available
for existing streams and text-oriented payloads. Brotli quality 11 uses window
24 and is checked against a pinned reference vector. Compression appears in
two places:

- **A payload compressed as a whole.** `File::add_coded(kind, "json",
  "zstd", name, bytes, n)` stores an object encoded `json+zstd` with its
  decoded size recorded; `File::read` hands the original back, and
  `File::read_stored` the bytes on disk. Files written by other writers with
  the same `inner+codec` convention read unchanged.
- **A table compressed per column.** `write_store(path, table,
  StoreOptions{"zstd"})` and `pto_add_store(..., options)` write a version 4
  `.dstore`: each blob transformed (delta for integers, byte shuffle for
  floats) and compressed on its own, so the directory, a column subset and a
  row window still read without touching the rest. A raw write stays version
  3, byte for byte.

```sh
pto add run.pto notes.json --codec zstd      # a coded object; pto cat decodes it
./build/ptobench | grep -E 'zstd|brotli'     # ratio and speed on a photon table
```

## Python

`python/` is the reference implementation in Python: `ptolib.pto`, one
standard-library-only file that reads containers the C++ library writes (two-index
commits, tags, annotations, cues, the banner, executable bundles, coded
payloads) and writes containers the header opens for editing, with a reader
over byte ranges for HTTP; and `ptolib.dstore`, the table with numpy, version
4 compression included. It is what a project vendors when it wants the format
without a compiled dependency: `scripts/vendor_py.sh <dest>` copies `pto.py`
with its provenance line. `pip install ./python[all]` installs the package with
numpy and the codecs; `python -m pytest python/tests` checks it against the
golden files and against the `pto` tool in `build/`, and `pto dump FILE` is
the JSON both implementations must agree on for any file.

```python
from ptolib import PtoReader, PtoWriter, read_store, write_store
with PtoWriter("run.pto", title="a measurement") as w:
    w.add_coded("meta.json", "table", payload, "zstd", inner_encoding="json")
with PtoReader("run.pto") as r:
    r.read("meta.json")                         # decoded
r = PtoReader.open_stream(fetch, size)          # fetch(offset, n): an HTTP Range
```

## Executable containers

`pto bundle run.pto -o run-x.pto` prepends a 12 KiB polyglot stub. On Linux and
macOS the result runs as a shell script that finds `pto` or `ptoview` and
opens itself; opened with the library it is an ordinary container whose EBML
header starts at `File::ebml_offset()`. See the specification's
*Executable containers* section.

## Vendoring

`scripts/vendor.sh <consumer-dir>` copies the buildable source package:
`include/`, `src/` (including codec adapters), `cmake/`, `thirdparty/`, and the
root build, license and README files. `VENDORING.json` records a SHA-256 for each
managed file; a refresh refuses to overwrite local edits. `--link` creates
symlinks for development across checkouts and is unsuitable for committed
vendor copies. Development tools and tests remain in the upstream checkout;
consume a vendored package with `add_subdirectory()` and `ptolib::ptolib`.
The source-only package can also be configured independently; development
tools, tests, and benchmarks default to `OFF` when their sources are absent.

Migrating a previous header copy requires updating the vendor directory,
removing the consumer's implementation macro, and linking the compiled target.
The source-layout change does not change PTO or DataStore encodings.
`scripts/vendor_py.sh <dest>` independently refreshes the Python reader.

### Optional single-header artifact

For environments that require one C++ header, generate a release artifact from
the maintained sources:

```sh
python3 tools/embed_codecs.py --output dist/ptolib.h
python3 tools/embed_codecs.py --output dist/ptolib.h --check
```

The default output is `dist/ptolib.h`; generation never rewrites the maintained
public header. The artifact includes the same core and adapter implementation,
plus codec sources, and is intentionally large. Compile it once:

```cpp
#define PTOLIB_IMPLEMENTATION
#include "ptolib.h"  // generated dist/ptolib.h, not include/ptolib/ptolib.h
```

This implementation still needs `nlohmann/json.hpp`; `PTOLIB_JSON_INCLUDE` can
select another include spelling. Artifact macros `PTOLIB_WITH_ZSTD`,
`PTOLIB_WITH_BROTLI`, `PTOLIB_WITH_LZ4`, and `PTOLIB_WITH_DEFLATE` select system
libraries that the consumer must link. `PTOLIB_NO_<CODEC>` disables a codec;
`PTOLIB_DECODE_ONLY` or `PTOLIB_<CODEC>_DECODE_ONLY` removes encoder callbacks,
with the same bundled Brotli/miniz source omission described above. These are
compiler macros for the artifact, separate from the CMake provider settings.

## Tools

- `pto` -- `ls | tree | info | tags | cat | extract | verify | pack | add |
  columns | groups | head | bundle | ui`. `pto FILE.pto` lists it; `pto verify
  FILE.pto` checks the framing with this reader's own walker.
- `ptoview` -- a terminal viewer over a container's table of contents.
- `pto_ebml_check` (option `PTOLIB_BUILD_EBML_CHECK`, needs libebml 1.x) --
  validates a container with the reference EBML implementation, which is the
  check that does not trust this code.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options: `PTOLIB_BUILD_TOOLS`, `PTOLIB_BUILD_TESTS`, `PTOLIB_BUILD_BENCH` (all
`ON` for a full top-level checkout, `OFF` for source-only packages or when
included as a subdirectory),
`PTOLIB_BUILD_EBML_CHECK` (`OFF`), `PTOLIB_JSON_INCLUDE_DIR` (empty uses the copy
under `thirdparty/`). Codec provider and decoder-only options are described above.

The tests are plain C++ programs plus a pytest file for the CLI
(`python -m pip install pytest`):

| Test | What it proves |
|---|---|
| `test_container` | the container: create, add, update in place, relocate, remove, tags, annotations, cues, index generations, capacity refusal, compaction, one-index and two-index golden files |
| `test_datastore` | the table: columns, masks, groups, selections, the expression engine |
| `test_store_file` | the `.dstore` encoding: round trips, partial reads, version 1-3 golden files |
| `test_simd` | every SIMD tier the machine has gives the same bits as the base tier |
| `test_codec` | codec capabilities, coded objects, and version 4 stores: round trips, partial reads, disabled/reader-only refusals, and bundled Brotli parity |
| `test_codec_stream` | valid and truncated Zstandard streams without a known decoded size |
| `tests/test_amalgamation.py --compile` | deterministic artifact generation, source preservation, and C++14 full, reader-only, disabled, and LZ4-only builds |
| `test_tool` | the `pto` command line, including executable bundles |
| `python/tests` | the Python reference against the golden files, itself, and the `pto` tool in both directions |
| `test_conformance` (in `python/tests`) | the two implementations do not diverge: `pto dump` and `ptolib.dump` produce the same document for every file in a corpus written by both, the canonical raw table is byte-identical from both writers, and each reads and edits the other's containers. The corpus uses only the codecs both sides have (`pto codecs`) |

CI (GitHub Actions, `.github/workflows/ci.yml`) builds and tests bundled codecs
on Linux (GCC and Clang), macOS (arm64), and Windows (MSVC, SSE2 and AVX2
baselines). Additional Linux builds exercise system, disabled, and decoder-only
codec configurations. The suite also covers sanitizers, C++14 declarations and
core implementation with warnings as errors, generated artifact consumers, the
installed C++14 package, AVX2 kernel presence, SWIG parsing, and Doxygen. Tagged
releases include the modular source package, optional single-header artifact,
and tool binaries.

## Documentation

`doxygen Doxyfile` writes the API reference to `build/docs/html`. Every public
type and function in the header is documented where it is declared; the
reference is generated from those comments and from `docs/`.

## Versioning

`PTOLIB_VERSION_MAJOR/MINOR/PATCH/STRING` in the header; git tags `vX.Y.Z`.
The on-disk formats are versioned separately (`DocTypeVersion` in the EBML
header, the `.dstore` header's version word) and only ever grow. A file
written by any released version is read by every later one.

## License

MIT. Copyright (c) 2026 Thomas-Otavio Peulen.
