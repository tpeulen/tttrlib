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
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretNumerics.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretMixture.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretPopulations.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretEstimators.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretAuto.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretUncertainty.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretBootstrap.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretMixtureNd.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretClassifyNd.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretOutliers.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretHdbscan.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretSpecies.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretRegistry.cpp"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretRcm.cpp"
    HEADERS  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    DEPENDS  core util math
    SWIG_INTERFACES SpectralCrosstalk.i BackgroundEstimation.i AccurateFret.i
    TEST_DIR test/python/corrections
)

# The accurate-FRET kernels are A/B-pinned against numpy, which rounds every
# product before the add. A contracted a*b+c (clang's default on arm64) moves
# the last bit, and EM plus the self-consistent calibration carry it along.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretEs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretGeneral.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretNumerics.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretMixture.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretPopulations.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretEstimators.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretAuto.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretUncertainty.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AccurateFretBootstrap.cpp"
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
