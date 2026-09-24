# PRD-041 — tttrlib as a Pyodide wheel (wasm32-emscripten)

> **PRD #:** 041 · **Status:** 🟡 In Progress · **Created:** 2026-09-23 · **Owner:** tpeulen
> **Related:** PRD-016 (JavaScript bindings — its *Non-goals* put browser/WASM
> out of scope and its *Fork decision* left the door open through
> emscripten + emnapi; this PRD **supersedes that out-of-scope note** by taking
> the other door: the Python bindings, compiled by emscripten, loaded by Pyodide),
> PRD-019 (DataStore — the thing the browser host needs), PRD-020 (PTO reads)
>
> **Where it stands (2026-09-23).** `tools/pyodide/build_wheel.sh` builds
> `tttrlib-0.27.0-cp313-cp313-pyodide_2025_0_wasm32.whl` (3.0 MB, a 11.7 MB
> `_tttrlib` wasm module) in about three minutes on an M-series Mac, and
> `tools/pyodide/smoke_test.mjs` passes under the npm `pyodide@0.28.0` package in
> Node. The first build also turned up a real wasm32 bug in the correlator, which
> is now fixed (see *Results*). What is still open: CI.

## Summary

ndXplorer is moving off PyQt onto emtk and into the browser, the way chimol
already runs there: Pyodide in the page, WebGPU for drawing. Its data model is
`tttrlib.DataStore`, so ndXplorer in a page needs tttrlib in a page. Two ways
were on the table — replace the DataStore with a numpy table on the web side, or
build tttrlib for wasm. The first forks ndXplorer's data layer in two. This PRD
is the second: one `tools/pyodide/build_wheel.sh` that turns this tree into a
`tttrlib-*-cp313-cp313-pyodide_2025_0_wasm32.whl`, installable into Pyodide 0.28
with `micropip.install` or `pyodide.loadPackage`.

## Why the Python bindings and not PRD-016's emnapi path

PRD-016 named emscripten + emnapi as "the credible way to get tttrlib into a
page". That gives JavaScript a tttrlib. ndXplorer is Python; in the browser it
runs on Pyodide, and what it imports is the Python module, including the pure
Python half of the package (`DataStore.py`, `TTTR.py`, the histogram and
selection helpers) that the JavaScript binding does not have. Compiling the
existing SWIG Python wrapper with emscripten reuses every `.i` file and every
Python helper as they are; the JavaScript binding would need the helpers ported.

## Pinned versions

| Piece | Version | Why this one |
|---|---|---|
| Pyodide | 0.28.0 | What `chimol/hosts/web/index.html` loads from the CDN. |
| emscripten | 4.0.9 | Not a free choice: the ABI Pyodide 0.28 was built with, read back from the cross-build environment (`pyodide config get emscripten_version`). |
| Python | 3.13 (target 3.13.2) | Pyodide 0.28's interpreter. |
| pyodide-build | 0.30.9 | Writes the `pyodide_2025_0_wasm32` tag. 0.39 writes the PEP 783 `pyemscripten_2025_0_wasm32` tag, which micropip 0.10 (shipped with Pyodide 0.28) does not recognise. |

The toolchain installs outside the checkout: a venv at `~/opt/pyodide-venv`,
emsdk at `~/opt/emsdk`, the cross-build environment at
`~/opt/pyodide-xbuildenv`. All three are overridable by environment variable,
and the script installs whichever is missing.

## What the wasm build turns off, and how

Everything is in one `[[tool.scikit-build.overrides]]` block in
`pyproject.toml`, selected by `PYODIDE=1`, which pyodide-build exports into the
build environment. It appends to the native `cmake.args`, so it only says what
differs:

| Setting | Why |
|---|---|
| `BUILD_PHOTON_HDF=OFF` | There is no wasm HDF5 to link. This was already a clean CMake option: `io_hdf5` and `io_hdf5_table` still build and every entry point fails cleanly, so nothing above them compiles twice. Photon-HDF5 reading/writing and the HDF5 DataStore backend are unavailable in the browser; `.dstore`, `.pto` and CSV are the persistence formats there. |
| `WITH_OPENMP=OFF` | Pyodide has no threads. |
| `TTTRLIB_MODULE_TYPE=STATIC` | One self-contained `_tttrlib` side module instead of 30-odd module libraries that the dynamic loader would have to resolve from the wheel directory. `tttrlib_install_modules` now installs nothing in a STATIC build, so no `.a` files end up in the wheel. |
| `TTTRLIB_LTO=OFF` | STATIC module objects are compiled `-fno-lto` anyway. |
| `install.strip=false` | Keep the wasm module exactly as emscripten linked it. |

libtiff (bundled, static), pocketfft, nlohmann_json and ptolib with its bundled
zstd/brotli/lz4/deflate codecs all build for wasm unchanged.

One fix was needed in the tree for any cross-compiled wheel: the extension
suffix came from asking the build interpreter for `EXT_SUFFIX`, which answers
for the build host. The Pyodide wheel got `_tttrlib.cpython-313-darwin.so`,
which the wasm interpreter never tries to import. `ext/CMakeLists.txt` now takes
the SOABI from scikit-build-core's `SKBUILD_SOABI` when it is set.

## Acceptance criteria

1. `tools/pyodide/build_wheel.sh` on a clean machine (with `uv` or `python3.13`,
   git, cmake, ninja and swig) produces
   `dist/pyodide/tttrlib-<version>-cp313-cp313-pyodide_2025_0_wasm32.whl`.
2. `node tools/pyodide/smoke_test.mjs <wheel>` under the npm `pyodide@0.28.0`
   package loads numpy and the wheel, imports tttrlib, builds a DataStore from
   `bh/bh_spc132_sm_dna/sliding_window_All 0.1500#60/bi4_bur/m000.bur` and reads a
   column, and opens `bh/bh_spc132_sm_dna/m000.pto` as a `TTTR`.
3. The native wheel is unchanged: the override block is inert without
   `PYODIDE=1`, and the `SKBUILD_SOABI` suffix is the same string the
   interpreter reported on native builds.

## Results

Smoke test (`node tools/pyodide/smoke_test.mjs dist/pyodide/tttrlib-0.27.0-cp313-cp313-pyodide_2025_0_wasm32.whl`):

```
pyodide 0.28.0
tttrlib 0.27.0 numpy 2.2.5
[ok]   DataStore in memory: [0.0, 1.0, 2.0, 3.0, 4.0]
[ok]   DataStore from .bur (read_csv): 11 rows x 37 cols; 'Number of Photons'[:5]=[0, 1951, 0, 1957, 0]
[ok]   TTTR from .pto: 1968899 photons, macro_times[:3]=[60675, 72854, 93374], channels=[0, 1, 8, 9]
[ok]   DataStore.histogram: HistogramNd
[ok]   Correlator (64-bit macro times): g[1:3]=[1.3702, 1.8684] g[-1]=1.0539
```

The `.pto` photon count, first macro times and channels match the native
build reading the same file. Also checked by hand, not in the smoke test: the
`.spc` reader (174 438 photons, same as native), a sliding-window burst search
(2898 bursts, same as native), a 2-million-row `write_csv`/`read_csv` round
trip, `TTTR.write` to `.ptu`, TIFF enums present, and the HDF5 writer returning
`False` with "Not built with Photon HDF interface".

**Threads.** Pyodide reports one CPU, so every `std::thread` pool in the tree
(CSV reader/writer, histogram, correlator, burst searches) takes its
single-thread path and never spawns. Nothing had to change for that.
`TTTRStreamWriter`'s background writer thread is the one path that always
spawns; it is not exercised and would fail to start in Pyodide.

**The wasm32 bug.** The first wheel's correlation curve was wrong at short
lags (0.0, 0.1246, ... against 1.3702, 1.8684 natively) and right at long ones.
The Wahl kernel held the lag offset and the bin edges, which are macro times
(`unsigned long long`), in `size_t`, 32 bits on wasm32. Macro times past 2^32
wrapped and the short-lag pairs landed in the wrong bins. They are now
`unsigned long long` in all four kernels (scalar/AVX/NEON Wahl and the species
matrix). The change is a no-op on 64-bit targets. This bug would hit any
32-bit build, not only wasm. The smoke test asserts the two short-lag values so
a regression shows up.

A grep for other `size_t` copies of macro times in `modules/` turned up nothing
else. That is a grep, not an audit: any other kernel that indexes by a
64-bit time could have the same bug and should be checked against native.

## Out of scope / follow-ups

- **CI.** No workflow builds the wasm wheel yet. A job would run the build
  script on ubuntu and the smoke test with the tttr-data fixtures.
- **Size.** The wheel is built `-O3` like the native one. `-Os`/`-Oz` for the
  wasm side is untried.
- **The split bindings** (`TTTRLIB_PYTHON_SPLIT`) are not built for wasm; the
  single wrapper is what the web host loads.
- **HDF5 in the browser.** Would need an emscripten HDF5 (Pyodide ships `h5py`
  with one). Not needed for ndXplorer, whose files are `.bur`, CSV, `.dstore` and
  `.pto`.
