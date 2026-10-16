# spike_ui: the I1 product UI spike (docs/spikes/i1-ui.md, ADR-0005). The quick catalog mock screen of rv_ui (RmlUi
# on SDL3 and OpenGL 3) over the test room (rv_app's SceneGraphics, unchanged), driven by a scripted virtual gamepad:
# every focus change logged, the UI pass timed on the CPU and the GPU (p50/p99 per phase, quiet frames and frames with
# changes), its heap allocations counted, a resize to 1280x720 and back, a JSON report (--report) and screenshots at
# chosen frames (--screenshot-at <frame>:<png>).

add_executable(spike_ui
    src/allocation_counter.cpp
    src/main.cpp
    src/mock_catalog.cpp
    src/scenario.cpp
    src/spike_app.cpp
    src/spike_options.cpp
    src/spike_report.cpp
    src/virtual_pad.cpp
)
rv_set_target_defaults(spike_ui)
target_link_libraries(spike_ui PRIVATE rv_ui rv_app SDL3::SDL3 nlohmann_json::nlohmann_json)
# Developer builds read the assets from the source tree: the document reloads when saved.
if(RV_DEV_ASSET_ROOT)
    target_compile_definitions(spike_ui PRIVATE RV_SPIKE_WATCH_DOCUMENTS=1)
endif()
rv_deploy_assets(spike_ui)

# GPU check (label "gpu", run by the *-gpu test presets): the whole scripted scenario (1910 frames) in a hidden
# 1920x1080 window must pass every check: pad-only navigation, stable focus, the card, the chips, the resize, text
# scaling and high contrast, a clean RmlUi log, the screenshots. --smoke also fails the run when it stops before its last
# frame or the OpenGL debug output reports a high-severity message (Debug builds), and the spike fails it when a check
# of the scenario never ran.
#
# Test registration: the top-level CMakeLists.txt adds spikes/ before it calls enable_testing(), so neither spikes/ nor
# this directory would get a CTestTestfile and ctest would never see the case. This directory enables testing for
# itself and, through CMAKE_TESTING_ENABLED (what enable_testing() sets), for spikes/, whose scope then carries it into
# the spike directories added after this one (spikes/CMakeLists.txt adds video, ui, launch in that order): launch's
# tests are registered by accident, video's are not unless it enables testing itself. The fix belongs to the top level,
# at the merge of the I1 spikes: call enable_testing() before add_subdirectory(spikes) and delete the PARENT_SCOPE line
# below.
if(NOT RV_BUILD_TESTS)
    return()
endif()
enable_testing()
set(CMAKE_TESTING_ENABLED ON PARENT_SCOPE)
set(RV_SPIKE_UI_WORK_DIR "${CMAKE_BINARY_DIR}/spikes/ui")
add_test(NAME gpu.spike_ui.scenario
    COMMAND spike_ui --smoke --frames 1910 --hidden --no-vsync --fixed-dt 0.016667
            --data-dir "${RV_SPIKE_UI_WORK_DIR}/data"
            --report "${RV_SPIKE_UI_WORK_DIR}/scenario_report.json"
            --run-report "${RV_SPIKE_UI_WORK_DIR}/scenario_run.json"
            --screenshot-at "108:${RV_SPIKE_UI_WORK_DIR}/down10.png"
            --screenshot-at "112:${RV_SPIKE_UI_WORK_DIR}/actions.png"
            --screenshot-at "682:${RV_SPIKE_UI_WORK_DIR}/resized_1280x720.png"
            --screenshot-at "806:${RV_SPIKE_UI_WORK_DIR}/last_chip.png"
            --screenshot-at "908:${RV_SPIKE_UI_WORK_DIR}/text_scale_1.2.png"
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set_tests_properties(gpu.spike_ui.scenario PROPERTIES LABELS gpu TIMEOUT 600 RUN_SERIAL TRUE)
