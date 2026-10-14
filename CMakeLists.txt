cmake_minimum_required(VERSION 3.28)

project(Retroverse
    VERSION 0.1.0
    DESCRIPTION "Explorable 3D frontend for ES-DE game collections"
    HOMEPAGE_URL "https://github.com/pingumen96/retroverse"
    LANGUAGES C CXX)

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

# --- Project ------------------------------------------------------------------------------------
add_subdirectory(third_party/glad)
add_subdirectory(src)

if(RV_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(RV_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)
    add_subdirectory(tests)
endif()

message(STATUS "Retroverse ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | dev UI: ${RV_DEV_UI} | "
               "GL debug: ${RV_GL_DEBUG} | tests: ${RV_BUILD_TESTS} | tools: ${RV_BUILD_TOOLS}")
