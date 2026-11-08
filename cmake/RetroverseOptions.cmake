# Build options for Retroverse. Presets in CMakePresets.json set the per-configuration defaults.

option(RV_BUILD_TESTS "Build unit tests (GPU-free) and GPU smoke tests" ON)
# RV_BUILD_TOOLS builds tools/: retroverse-cli (the catalog of M1 from the command line) and retroverse-screenshot-diff
# in every configuration, retroverse-asset-preview with RV_DEV_UI. Turned off, it also drops the unit tests of their
# GPU-free cores and the GPU smoke tests, which compare screenshots with the screenshot diff.
option(RV_BUILD_TOOLS "Build retroverse-cli, retroverse-screenshot-diff and retroverse-asset-preview (RV_DEV_UI)" ON)
option(RV_BUILD_SPIKES "Build the risk-spike measurement executables under spikes/ (docs/roadmap.md, I1)" ON)
# RV_ENABLE_XR builds what needs the OpenXR loader (vcpkg feature "xr", enabled by every preset): the I10 spike, later
# the VR mode (ADR-0017).
option(RV_ENABLE_XR "Build the OpenXR parts (vcpkg feature xr): the I10 spike and the VR mode" ON)
option(RV_DEV_UI "Build the Dear ImGui developer overlay" ON)
option(RV_GL_DEBUG "Request a debug OpenGL context and route GL_DEBUG_OUTPUT to the log" ON)
option(RV_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)
option(RV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in project targets" ON)
option(RV_DEV_ASSET_ROOT "Resolve assets from the source tree first (shader hot-reload edits the real files)" ON)
# RV_USE_PCH gives every first-party target a precompiled header of the standard library and of the third-party headers
# it can reach (cmake/TargetDefaults.cmake, rv_apply_precompiled_headers). Turned off, every file compiles from its own
# includes alone: a build without it finds an include a file forgot and the precompiled header supplied.
option(RV_USE_PCH "Precompile the standard and third-party headers of every first-party target" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
