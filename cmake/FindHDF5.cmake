# FindHDF5.cmake - HDF5 finder and configuration for tttrlib
# Uses traditional finding via find_package(HDF5).

include(FindPackageHandleStandardArgs)

if(BUILD_PHOTON_HDF)
    add_compile_definitions(BUILD_PHOTON_HDF)

    message(STATUS "Using traditional HDF5 sources (vcpkg/conda-forge/system)")
    
    # Prevent infinite recursion by removing local cmake dir from CMAKE_MODULE_PATH
    set(_ORIGINAL_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
    list(REMOVE_ITEM CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    
    find_package(HDF5 REQUIRED COMPONENTS C)

    # Restore CMAKE_MODULE_PATH
    set(CMAKE_MODULE_PATH ${_ORIGINAL_CMAKE_MODULE_PATH})

    # thirdparty/HighFive hard-codes the imported target name HDF5::HDF5,
    # which is what CMake's own module-mode FindHDF5.cmake creates. When
    # find_package(HDF5) above resolves via a package's own CONFIG file
    # instead (e.g. CMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE, or a CMake build
    # whose bundled FindHDF5.cmake delegates to CONFIG mode internally), the
    # target comes from that package's own naming instead -- vcpkg's hdf5
    # exports hdf5::hdf5-shared (or hdf5::hdf5-static for a static build),
    # not HDF5::HDF5, and HighFive's target_link_libraries() then fails with
    # "the target was not found". Alias whichever CONFIG-mode target exists.
    if(NOT TARGET HDF5::HDF5)
        if(TARGET hdf5::hdf5-shared)
            add_library(HDF5::HDF5 ALIAS hdf5::hdf5-shared)
        elseif(TARGET hdf5::hdf5-static)
            add_library(HDF5::HDF5 ALIAS hdf5::hdf5-static)
        endif()
    endif()
    
    set(HDF5_IS_FROM_H5PY FALSE)
    set(HDF5_DLL_PATHS "")

    message(STATUS "HDF5 version: ${HDF5_VERSION}")
    message(STATUS "HDF5 libraries: ${HDF5_LIBRARIES}")
    message(STATUS "HDF5 headers: ${HDF5_INCLUDE_DIRS}")

else()
    message(STATUS "HDF5 support disabled (BUILD_PHOTON_HDF=OFF)")
    set(HDF5_IS_FROM_H5PY FALSE)
    set(HDF5_DLL_PATHS "")
endif()
