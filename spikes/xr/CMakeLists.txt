# spikes/xr (I10 step 0, ADR-0017): an OpenXR session on Retroverse's OpenGL 4.5 context through XR_KHR_opengl_enable,
# a ray-cast room in the headset, frame pacing measured against the runtime's display period.
#   spike_xr_core   GPU-free part: the off-axis projection of an eye's field and the view of its pose, the frame
#                   statistics (missed refreshes, CPU and GPU time), the choice of the swapchain format
#   spike_xr        the measurement executable: rv_platform's window and context, rv_render's program and state cache,
#                   the OpenXR loader, a JSON report
#   spike_xr_tests  unit tests of spike_xr_core (label "unit")
# No automated GPU case: a session needs a headset and its runtime, so spike_xr runs by hand (docs/spikes/i10-xr.md).

if(NOT RV_ENABLE_XR)
    return()
endif()

add_library(spike_xr_core STATIC
    core/src/frame_stats.cpp
    core/src/swapchain_format.cpp
    core/src/xr_math.cpp
)
rv_set_target_defaults(spike_xr_core)
target_include_directories(spike_xr_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
target_link_libraries(spike_xr_core PUBLIC rv_core glm::glm nlohmann_json::nlohmann_json)
rv_forbid_link(spike_xr_core EXTRA OpenXR::openxr_loader)

add_executable(spike_xr
    src/main.cpp
    src/room_scene.cpp
    src/xr_session.cpp
)
rv_set_target_defaults(spike_xr)
# opengl32: wglGetCurrentDC and wglGetCurrentContext, the handles XR_KHR_opengl_enable takes.
target_link_libraries(spike_xr PRIVATE
    spike_xr_core rv_core rv_platform rv_render OpenXR::openxr_loader opengl32)

if(RV_BUILD_TESTS)
    add_executable(spike_xr_tests
        tests/test_frame_stats.cpp
        tests/test_xr_math.cpp
    )
    rv_set_target_defaults(spike_xr_tests)
    target_link_libraries(spike_xr_tests PRIVATE spike_xr_core Catch2::Catch2WithMain)
    rv_forbid_link(spike_xr_tests EXTRA OpenXR::openxr_loader)
    catch_discover_tests(spike_xr_tests
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "unit."
        WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        PROPERTIES LABELS unit)
endif()
