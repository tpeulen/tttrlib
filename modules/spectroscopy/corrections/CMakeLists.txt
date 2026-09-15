# SPDX-License-Identifier: BSD-3-Clause
#
# corrections -- crosstalk, background estimation, max-entropy inversion
#
tttrlib_add_module(
    NAME     corrections
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/SpectralCrosstalk.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/BackgroundEstimation.cpp"
    HEADERS  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    DEPENDS  core util math
    SWIG_INTERFACES SpectralCrosstalk.i BackgroundEstimation.i
    TEST_DIR test/python/corrections
)
