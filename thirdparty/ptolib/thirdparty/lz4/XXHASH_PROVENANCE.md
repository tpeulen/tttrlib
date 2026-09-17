# xxHash implementation

`xxhash.c` is the unmodified upstream xxHash v0.6.5 implementation:
https://github.com/Cyan4973/xxHash/blob/v0.6.5/xxhash.c

It matches the v0.6.5 `xxhash.h` already vendored with LZ4. The source is
required to compile LZ4 frame support independently of the Zstandard codec.
Both files contain their upstream BSD 2-Clause license notices.
