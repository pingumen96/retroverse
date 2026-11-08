cmake_minimum_required(VERSION 3.28)

# vcpkg's find_package override is a macro that project() defines when it loads the toolchain, and a macro runs with
# the policies recorded where it was defined: find modules called through it see the policies set before project(),
# not those set after it. vcpkg's FindFFMPEG (below) calls cmake_parse_arguments(PARSE_ARGV) with empty values, which
# warns while CMP0174 (CMake 3.31) is unset.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# Before project(), which loads the vcpkg toolchain: skips its manifest install when nothing it depends on changed.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VcpkgManifestStamp.cmake")

project(Retroverse
    VERSION 0.1.0
    DESCRIPTION "Explorable 3D frontend for ES-DE game collections"
    HOMEPAGE_URL "https://github.com/pingumen96/retroverse"
    LANGUAGES C CXX)
rv_vcpkg_manifest_stamp_write()

if(NOT MSVC)
    message(FATAL_ERROR
        "Retroverse currently targets MSVC (Visual Studio 2026) on Windows x64. "
        "Configure with one of the msvc-x64-* presets from a VS developer shell (see scripts/dev.ps1).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RetroverseOptions)
include(TargetDefaults)
include(LayerRules)
include(AssetDeploy)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
# Retroverse uses no C++20 modules: without the dependency scan, a file compiles as soon as its target's dependencies
# are built, instead of after a scan of every file of its target and their collation.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Executables, vcpkg DLLs (copied by the vcpkg applocal step) and deployed assets live side by side.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# --- Third-party packages (vcpkg manifest mode) -------------------------------------------------
find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
if(RV_DEV_UI)
    find_package(imgui CONFIG REQUIRED)
endif()
if(RV_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

# FFmpeg (vcpkg feature "video", enabled by every preset): avformat, avcodec, swscale, swresample and avutil only, LGPL
# components linked as DLLs. vcpkg's FindFFMPEG sets variables; rv::ffmpeg wraps them in one target (CMP0174 is set
# before project(), above).
find_package(FFMPEG REQUIRED)
add_library(rv_ffmpeg INTERFACE)
add_library(rv::ffmpeg ALIAS rv_ffmpeg)
target_include_directories(rv_ffmpeg SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(rv_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(rv_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})

# RmlUi (vcpkg feature "ui", enabled by every preset) with the FreeType font engine; its SDL3 and OpenGL 3 backends are
# vendored in third_party/rmlui_backends on our glad loader.
find_package(RmlUi CONFIG REQUIRED)

# SQLite with FTS5 and the JSON functions (vcpkg features fts5 and json1): the catalog database, private to
# rv_catalog_db (ADR-0008).
find_package(unofficial-sqlite3 CONFIG REQUIRED)
# pugixml: ES-DE's XML files, private to rv_esde; parsed with parse_fragment and never through XPath, which throws
# (ADR-0008).
find_package(pugixml CONFIG REQUIRED)
# lunasvg (with plutovg): the systems' SVG logos read from ES-DE's themes, private to rv_assets' svg_raster.cpp
# (ADR-0019 §3).
find_package(lunasvg CONFIG REQUIRED)
# The OpenXR loader (vcpkg feature "xr", enabled by every preset): OpenXR::openxr_loader, with openxr/openxr.h and
# openxr/openxr_platform.h; it finds the active runtime (Meta Horizon Link, SteamVR, ...) at run time (ADR-0017).
if(RV_ENABLE_XR)
    find_package(OpenXR CONFIG QUIET)
    # A tree configured before its preset gained the feature keeps the old list in its cache, whose manifest install takes
    # the port out of the shared build/vcpkg_installed: a bare `cmake --build` that regenerates ends here.
    if(NOT OpenXR_FOUND)
        message(FATAL_ERROR
            "Could not find OpenXR, which the VR mode needs (vcpkg feature \"xr\"): this tree's "
            "VCPKG_MANIFEST_FEATURES is \"${VCPKG_MANIFEST_FEATURES}\". Configure it again with its preset, "
            "./scripts/dev.ps1 cmake --preset <preset> (./scripts/build.ps1 and ./scripts/gate.ps1 do it themselves), "
            "or set RV_ENABLE_XR=OFF to build without VR.")
    endif()
endif()

# Testing is enabled before any directory is added: tests/ and spikes/ register ctest cases, and a directory only gets a
# test file when testing is already enabled while it is configured.
if(RV_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)
endif()

# --- Project ------------------------------------------------------------------------------------
# Order: src/ (the modules), tools/ (whose GPU-free cores the tests use), tests/ (with the fake emulator), spikes/.
add_subdirectory(third_party/glad)
add_subdirectory(src)

if(RV_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(RV_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(RV_BUILD_SPIKES)
    # The launch spike starts the fake emulator of the tests, built here when the tests are not.
    if(NOT TARGET rv_fake_emulator)
        add_subdirectory(tests/fake_emulator)
    endif()
    add_subdirectory(spikes)
endif()

# Every first-party target and its links are known here.
rv_apply_precompiled_headers()

message(STATUS "Retroverse ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | dev UI: ${RV_DEV_UI} | "
               "GL debug: ${RV_GL_DEBUG} | XR: ${RV_ENABLE_XR} | tests: ${RV_BUILD_TESTS} | tools: ${RV_BUILD_TOOLS}")
