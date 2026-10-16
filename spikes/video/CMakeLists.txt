# spikes/video: I1 spike (c) (docs/spikes/i1-video.md, ADR-0004). spike_video plays videos decoded with FFmpeg on worker
# threads (rv_media) on textured quads, uploaded through persistently mapped pixel buffers (gl::VideoTexture), on the
# host of rv_app (AppHost: window, OpenGL context, debug output, frame loop), and writes a JSON report of its
# measurements.

add_executable(spike_video
    src/main.cpp
    src/spike_fixtures.cpp
    src/spike_options.cpp
    src/system_metrics.cpp
    src/texture_self_test.cpp
    src/video_spike_app.cpp
)
rv_set_target_defaults(spike_video)
target_link_libraries(spike_video PRIVATE rv_app rv_media rv_render rv_platform)
set_target_properties(spike_video PROPERTIES FOLDER "spikes")

if(RV_BUILD_TESTS)
    # Automated checks (label "gpu"): check_spike_video.cmake runs spike_video hidden on fixtures it generates under the
    # build tree and checks its exit code, report and screenshot.
    #   smoke   two 720p30 clips for 3 s: both play on time, the screenshot shows the frames uploaded last, no GL error,
    #           VideoTexture's resize and busy-ring paths pass their self-test; timing thresholds when the machine is
    #           mostly idle
    #   errors  the damaged files of SC-08 beside a good clip: each falls back for its reason, none crashes the run
    # The top-level CMakeLists.txt adds spikes/ before it enables testing, so a test added here would not be registered:
    # the tests are added at the end of the top-level directory instead (deferred), where testing is enabled.
    function(rv_spike_video_add_check name mode source_dir binary_dir)
        add_test(NAME "${name}"
            COMMAND "${CMAKE_COMMAND}" "-DSPIKE=$<TARGET_FILE:spike_video>" "-DWORK_DIR=${binary_dir}/${mode}"
                    "-DMODE=${mode}" -P "${source_dir}/check_spike_video.cmake")
        set_tests_properties("${name}" PROPERTIES LABELS gpu TIMEOUT 600)
    endfunction()
    # DEFER arguments are evaluated when the call runs, in the top-level directory: bake the values in now.
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL rv_spike_video_add_check
        [[gpu.spike/video: two generated 720p clips play on time for 3 s]] smoke
        [[${CMAKE_CURRENT_SOURCE_DIR}]] [[${CMAKE_CURRENT_BINARY_DIR}]])")
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL rv_spike_video_add_check
        [[gpu.spike/video: damaged files fall back to a picture without a crash]] errors
        [[${CMAKE_CURRENT_SOURCE_DIR}]] [[${CMAKE_CURRENT_BINARY_DIR}]])")
endif()
