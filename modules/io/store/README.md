# `io/store` — The Native `.dstore` File

Saves and reloads a [`DataStore`](../../core) (which lives in `core`) as a
`.dstore` file: full fidelity — dtypes, dictionary-encoded strings, bit-packed
masks, attributes — with no dependency on HDF5 or anything else. HDF5 is for
interoperability; this is for keeping what you had.

## Contents

- **`include/io_store.h`** — re-exports the `.dstore` API from ptolib (`thirdparty/ptolib/include/ptolib/ptolib.h`) under `tttrlib::io`.
- **`src/io_store.cpp`** — an empty translation unit; the implementation is the compiled `ptolib::ptolib` dependency that `core` links.

## Dependencies

- Depends on `io/base`, `core`.
