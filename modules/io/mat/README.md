# `io/mat` — MATLAB Level-5 MAT-files

Reads and writes the MAT-file format MATLAB uses by default (`-v6`/`-v7`;
not the HDF5-based `-v7.3`), including the zlib-compressed variables MATLAB
and Octave write since R14.

## Contents

- **`include/MatFile.h`** — `tttrlib::io::MatFile`: a file as a flat tree of
  arrays (numeric, logical, complex, char, cell, struct, object), read from a
  path or a buffer and saved plain or compressed. Payloads stay in their
  storage type and column-major order; the language layer gives them meaning.
- **`src/MatFile.cpp`** — element parsing (both tag forms, both byte orders),
  serialisation.
- **`src/Zlib.cpp`** — RFC 1950/1951 inflate (stored, fixed, dynamic blocks,
  Adler-32 checked) and a fixed-Huffman LZ77 deflate. No zlib dependency.

Python: `tttrlib.matfile.loadmat` / `savemat` / `whosmat`, scipy.io's names
and semantics (`squeeze_me`, `struct_as_record`, `chars_as_strings`,
`mat_dtype`, `simplify_cells`; `do_compression`, `long_field_names`,
`oned_as`).

Not supported: sparse arrays, function handles and opaque objects (a variable
of those classes is reported unreadable, the rest of the file still loads),
and `-v7.3` files (they are HDF5: read them with tttrlib's HDF5 support).

## Dependencies

- Depends on `io/base`.
