# Skips vcpkg's manifest install when nothing it depends on changed since the last one into the shared installed tree.
#
# vcpkg's toolchain runs `vcpkg install` on every configure, and a regeneration (a CMakeLists.txt edited, a source added)
# spends most of its time there (about 40 s on the development machine: the compiler's hash detected again, the ABI of
# every port computed again) only to find every package installed. The last successful install into
# VCPKG_INSTALLED_DIR (build/vcpkg_installed, shared by the presets of a checkout, ADR-0001) records a key of its inputs
# next to it; a configure whose key matches turns the install off for this run.
#
# The key holds the manifest (vcpkg.json and vcpkg-configuration.json), the manifest features, the triplets, the vcpkg
# executable's time stamp, the MSVC toolset version and the time stamp of the installed tree's status file, which every
# install or removal rewrites: another preset installing other features into the shared tree, a deleted tree or an
# updated vcpkg or compiler all run the install again. ./scripts/build.ps1 -Fresh removes the stamp; deleting it by hand
# (build/vcpkg_installed/.retroverse-manifest-stamp) does the same.
#
# Included before project(), which loads the toolchain; rv_vcpkg_manifest_stamp_write() runs after it.

set(_rv_vcpkg_stamp "")
set(_rv_vcpkg_key "")
if(VCPKG_INSTALLED_DIR AND DEFINED ENV{VCPKG_ROOT})
    set(_rv_vcpkg_stamp "${VCPKG_INSTALLED_DIR}/.retroverse-manifest-stamp")
endif()

function(_rv_vcpkg_manifest_key out_var)
    set(parts "")
    foreach(file IN ITEMS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-configuration.json")
        if(EXISTS "${file}")
            file(SHA256 "${file}" hash)
            list(APPEND parts "${file}=${hash}")
        endif()
    endforeach()
    set(status "${VCPKG_INSTALLED_DIR}/vcpkg/status")
    if(NOT EXISTS "${status}")
        set(${out_var} "" PARENT_SCOPE) # nothing installed: always install
        return()
    endif()
    file(TIMESTAMP "${status}" status_time "%Y-%m-%dT%H:%M:%S" UTC)
    file(TIMESTAMP "$ENV{VCPKG_ROOT}/vcpkg.exe" tool_time "%Y-%m-%dT%H:%M:%S" UTC)
    list(APPEND parts
        "features=${VCPKG_MANIFEST_FEATURES}"
        "triplet=${VCPKG_TARGET_TRIPLET}"
        "host=${VCPKG_HOST_TRIPLET}"
        "root=$ENV{VCPKG_ROOT}"
        "tool=${tool_time}"
        "toolset=$ENV{VCToolsVersion}"
        "status=${status_time}")
    string(SHA256 key "${parts}")
    set(${out_var} "${key}" PARENT_SCOPE)
endfunction()

# Every configure decides explicitly: the toolchain's CMAKE_DEPENDENT_OPTION forces the normal variable's value into the
# cache, so a cache left OFF by a skipped run never skips the next one.
set(_rv_vcpkg_skipped FALSE)
if(_rv_vcpkg_stamp)
    _rv_vcpkg_manifest_key(_rv_vcpkg_key)
    set(_rv_vcpkg_recorded "")
    if(EXISTS "${_rv_vcpkg_stamp}")
        file(READ "${_rv_vcpkg_stamp}" _rv_vcpkg_recorded)
        string(STRIP "${_rv_vcpkg_recorded}" _rv_vcpkg_recorded)
    endif()
    if(_rv_vcpkg_key AND _rv_vcpkg_key STREQUAL _rv_vcpkg_recorded)
        set(VCPKG_MANIFEST_INSTALL OFF)
        set(_rv_vcpkg_skipped TRUE)
        message(STATUS "vcpkg install skipped: the manifest, its features and the installed tree are unchanged")
        # The toolchain adds these only when it installs: an edited manifest still regenerates the build.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-configuration.json")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-configuration.json")
        endif()
    else()
        set(VCPKG_MANIFEST_INSTALL ON)
    endif()
endif()

# After project(): records the key of an install that just succeeded (the toolchain stops the configure on a failed one
# before project() returns).
function(rv_vcpkg_manifest_stamp_write)
    if(NOT _rv_vcpkg_stamp OR _rv_vcpkg_skipped OR Z_VCPKG_HAS_FATAL_ERROR)
        return()
    endif()
    _rv_vcpkg_manifest_key(key)
    if(key)
        file(WRITE "${_rv_vcpkg_stamp}" "${key}\n")
    endif()
endfunction()
