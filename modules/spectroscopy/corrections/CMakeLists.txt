# SPDX-License-Identifier: BSD-3-Clause
#
# corrections -- crosstalk, background estimation, accurate FRET
#
tttrlib_add_module(
    NAME     corrections
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/SpectralCrosstalk.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/BackgroundEstimation.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretEs.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretGeneral.cpp"
    HEADERS  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    DEPENDS  core util math
    SWIG_INTERFACES SpectralCrosstalk.i BackgroundEstimation.i AccurateFret.i
    TEST_DIR test/python/corrections
)
