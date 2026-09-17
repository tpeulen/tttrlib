# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(ptolib VERSION 0.4.0 LANGUAGES C CXX)
include(GNUInstallDirs)
# The header itself compiles as C++14 (a consumer builds it so); the tools and tests here use 17.

# The maintained library has a public header, a C++ implementation and
# separately compiled codecs. Single-header releases are generated on demand.

set(_ptolib_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set(_ptolib_top_level ON)
endif()
option(PTOLIB_BUILD_TOOLS "Build the pto CLI and the ptoview TUI" ${_ptolib_top_level})
option(PTOLIB_BUILD_TESTS "Build the tests" ${_ptolib_top_level})
option(PTOLIB_BUILD_EBML_CHECK "Build pto_ebml_check (needs libebml < 2)" OFF)
set(PTOLIB_JSON_INCLUDE_DIR "" CACHE PATH
    "Directory holding nlohmann/json.hpp; empty uses the copy under thirdparty/")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ptolib STATIC src/ptolib.cpp)
add_library(ptolib_impl ALIAS ptolib)
set_target_properties(ptolib PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 14
    CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF)
add_library(ptolib::ptolib ALIAS ptolib)
target_include_directories(ptolib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(ptolib PUBLIC cxx_std_14)

# nlohmann/json: the one dependency, for the msgpack column metadata. Only the
# implementation TU needs it, so it is a build-time include path here, not
# something exported to consumers -- they point PTOLIB_JSON_INCLUDE at theirs.
if(PTOLIB_JSON_INCLUDE_DIR)
    set(_ptolib_json_inc "${PTOLIB_JSON_INCLUDE_DIR}")
else()
    set(_ptolib_json_inc "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nlohmann_json/include")
endif()

target_include_directories(ptolib PRIVATE "${_ptolib_json_inc}")
include(cmake/Codecs.cmake)
ptolib_configure_codecs(ptolib)
if(NOT MSVC)
    target_compile_options(ptolib PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_codecs.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_custom_target(ptolib_amalgamation
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_codecs.py"
                --output "${CMAKE_CURRENT_BINARY_DIR}/dist/ptolib.h"
            VERBATIM)
    endif()
endif()

if(PTOLIB_BUILD_TOOLS)
    add_executable(pto tools/pto.cpp)
    target_link_libraries(pto PRIVATE ptolib_impl)
    add_executable(ptoview tools/ptoview.cpp)
    target_link_libraries(ptoview PRIVATE ptolib_impl)
    if(NOT MSVC)
        target_compile_options(pto PRIVATE -Wall -Wextra)
        target_compile_options(ptoview PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS pto ptoview RUNTIME DESTINATION bin)
endif()

if(PTOLIB_BUILD_EBML_CHECK)
    find_path(EBML_INCLUDE_DIR ebml/EbmlHead.h)
    find_library(EBML_LIBRARY NAMES ebml libebml)
    if(NOT EBML_INCLUDE_DIR OR NOT EBML_LIBRARY)
        message(FATAL_ERROR "PTOLIB_BUILD_EBML_CHECK=ON but libebml was not found (set CMAKE_PREFIX_PATH)")
    endif()
    add_executable(pto_ebml_check tools/pto_ebml_check.cpp)
    target_include_directories(pto_ebml_check PRIVATE "${EBML_INCLUDE_DIR}")
    target_link_libraries(pto_ebml_check PRIVATE "${EBML_LIBRARY}")
    install(TARGETS pto_ebml_check RUNTIME DESTINATION bin)
endif()

if(PTOLIB_BUILD_TESTS)
    enable_testing()
    if(PTOLIB_DEFLATE_PROVIDER STREQUAL "system")
        add_test(NAME test_system_codec_install
            COMMAND ${CMAKE_COMMAND}
                "-DPTOLIB_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DPTOLIB_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/system-codec-consumer"
                "-DPTOLIB_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/system_codec_install.cmake")
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_amalgamation
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_amalgamation.py"
                --compile --compiler "${CMAKE_CXX_COMPILER}" --json-include "${_ptolib_json_inc}")
        add_test(NAME test_vendor
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vendor.py")
    endif()
    add_test(NAME test_installed_consumer
        COMMAND ${CMAKE_COMMAND}
            "-DPTOLIB_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DPTOLIB_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-consumer"
            "-DPTOLIB_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DPTOLIB_GENERATOR=${CMAKE_GENERATOR}"
            "-DPTOLIB_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DPTOLIB_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DPTOLIB_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DPTOLIB_JSON_INCLUDE_DIR=${_ptolib_json_inc}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/installed_consumer.cmake")
    foreach(t test_container test_datastore test_store_file test_simd test_codec test_codec_stream test_codec_capabilities)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE ptolib_impl)
        target_compile_definitions(${t} PRIVATE
            PTOLIB_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            ${_ptolib_codec_defs})
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    # The conformance corpus writer: the canonical table and container the
    # Python reference builds too, so the two implementations are compared
    # file for file (python/tests/test_conformance.py).
    add_executable(conform_write tests/conform_write.cpp)
    target_link_libraries(conform_write PRIVATE ptolib_impl)
    if(PTOLIB_BUILD_TOOLS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME test_tool
                     COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:pytest-qt "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tool.py")
            set_tests_properties(test_tool PROPERTIES
                ENVIRONMENT "PTOLIB_PTO=$<TARGET_FILE:pto>;PTOLIB_TEST_DATA=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
            # The Python reference implementation against this build: the
            # golden files, round trips through the tool, and the conformance
            # corpus in both directions. Needs numpy; skips codecs it lacks.
            add_test(NAME test_python
                     COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:pytest-qt "${CMAKE_CURRENT_SOURCE_DIR}/python/tests")
            set_tests_properties(test_python PROPERTIES
                ENVIRONMENT "PTOLIB_PTO=$<TARGET_FILE:pto>;PTOLIB_CONFORM=$<TARGET_FILE:conform_write>;PTOLIB_TEST_DATA=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
        endif()
    endif()
endif()

# ---- install + package config -------------------------------------------------
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ptolib DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS ptolib EXPORT ptolibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT ptolibTargets NAMESPACE ptolib:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ptolib)
configure_package_config_file(cmake/ptolibConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ptolibConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ptolib)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ptolibConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ptolibConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ptolibConfigVersion.cmake"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SystemCodecs.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ptolib)

option(PTOLIB_BUILD_BENCH "Build ptobench, the performance scoreboard" ${_ptolib_top_level})
if(PTOLIB_BUILD_BENCH)
    add_executable(ptobench bench/bench.cpp)
    target_link_libraries(ptobench PRIVATE ptolib_impl)
    if(NOT MSVC)
        target_compile_options(ptobench PRIVATE -Wall -Wextra)
    endif()
endif()
