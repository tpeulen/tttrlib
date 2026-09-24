# ptolib source package

From https://github.com/tpeulen/ptolib, revision v0.3.2-6-g8f6eba1.
VENDORING.json records the exact SHA-256 of every managed source file.

Build with add_subdirectory() and link ptolib::ptolib. Codecs are built
separately; do not define PTOLIB_IMPLEMENTATION. Development tools and
tests remain in the upstream checkout. Fix upstream, then refresh with
scripts/vendor.sh. Unrelated destination files are preserved.
