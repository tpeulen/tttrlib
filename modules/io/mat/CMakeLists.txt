# SPDX-License-Identifier: BSD-3-Clause
#
# io_mat -- MATLAB Level-5 MAT-files (-v6/-v7), read and written. The flat
# node tree is here; numpy shaping and scipy's loadmat/savemat semantics are in
# tttrlib.matfile (ext/python/matfile_module.py).
#
# No EXTERNAL_DEPS: the zlib streams every MATLAB variable is compressed into
# are decoded by this module's own inflate/deflate (src/Zlib.cpp), so the
# module builds unchanged where no zlib is linked (WebAssembly, minimal builds).
tttrlib_add_module(
    NAME     io_mat
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/MatFile.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/Zlib.cpp"
    SWIG_INTERFACES MatFile.i
    HEADERS  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    DEPENDS  io
    TEST_DIR test/python
)
