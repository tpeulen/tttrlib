# SPDX-License-Identifier: BSD-3-Clause
#
# burstpipeline -- one native pass from a photon stream to a burst result table.
#
# Its own module, not part of `burst`, because it is the one place the burst
# search, the per-burst decays and the decay fits meet: `burst` does not depend
# on `decay` and should not start to just so a pipeline can live there.
tttrlib_add_module(
    NAME     burstpipeline
    SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/src/BurstPipeline.cpp"
    HEADERS  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    DEPENDS  util math core plugin burst decay
    EXTERNAL_DEPS tttrlib::json
    SWIG_INTERFACES BurstPipeline.i
    TEST_DIR test/python/burstpipeline
)
