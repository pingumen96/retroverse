# Build options for Retroverse. Presets in CMakePresets.json set the per-configuration defaults.

option(RV_BUILD_TESTS "Build unit tests (GPU-free) and GPU smoke tests" ON)
option(RV_BUILD_TOOLS "Build developer tools such as retroverse-asset-preview (requires RV_DEV_UI)" ON)
option(RV_DEV_UI "Build the Dear ImGui developer overlay" ON)
option(RV_GL_DEBUG "Request a debug OpenGL context and route GL_DEBUG_OUTPUT to the log" ON)
option(RV_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)
option(RV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project targets" ON)
option(RV_DEV_ASSET_ROOT "Resolve assets from the source tree first (shader hot-reload edits the real files)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
