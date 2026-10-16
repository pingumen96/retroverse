# spike_ui: the product UI spike (I1 spike d, docs/spikes/i1-ui.md, ADR-0005; I2 step 6, docs/i2/step-6.md). The quick
# catalog screen of rv_ui (RmlUi on SDL3 and OpenGL 3) over the test room (rv_app's SceneGraphics, unchanged), on a
# generated catalog of 100,000 games (a virtualized list over ui::InMemoryCatalogSource), driven through the UI actions
# of the action map by a scripted virtual gamepad (--scenario) or an input script (--input-script): every focus change
# and move of the selection logged, the UI pass timed on the CPU and the GPU (p50/p99 per phase, quiet frames and
# frames with changes), its heap allocations counted, a resize to 1280x720 and back, a JSON report (--report) and
# screenshots at chosen frames (--screenshot-at <frame>:<png>).

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

# GPU checks (label "gpu", run by the *-gpu test presets), in a hidden 1920x1080 window:
#  - gpu.spike_ui.scenario: the whole quick-catalog scenario must pass every check: pad-only navigation, stable focus,
#    the card, the chips, the virtualized list at its start, middle and end (searches, filters, pages, Home, End), the
#    resize, text scaling and high contrast, imports under the screen (lists a worker answers), a list emptied under the
#    card's actions, a clean RmlUi log, the screenshots (the list scrolled to its middle and its end among them). --smoke also fails the run when it stops before its last frame or the OpenGL debug output reports
#    a high-severity message (Debug builds), and the spike fails it when a check of the scenario never ran.
#  - gpu.spike_ui.input_script: tests/input_scripts/quick_catalog_select.json moves the selection through UI actions
#    alone (as --input-script drives retroverse.exe), ending where --expect-selection says.
#  - gpu.spike_ui.user_bindings: a user bindings.json (fixtures/user_bindings.json, copied into the run's data
#    directory) rebinds the UI actions the virtual pad then presses.
if(NOT RV_BUILD_TESTS)
    return()
endif()
set(RV_SPIKE_UI_WORK_DIR "${CMAKE_BINARY_DIR}/spikes/ui")
add_test(NAME gpu.spike_ui.scenario
    COMMAND spike_ui --smoke --frames 2271 --hidden --no-vsync --fixed-dt 0.016667
            --data-dir "${RV_SPIKE_UI_WORK_DIR}/data"
            --report "${RV_SPIKE_UI_WORK_DIR}/scenario_report.json"
            --run-report "${RV_SPIKE_UI_WORK_DIR}/scenario_run.json"
            --screenshot-at "108:${RV_SPIKE_UI_WORK_DIR}/down10.png"
            --screenshot-at "112:${RV_SPIKE_UI_WORK_DIR}/actions.png"
            --screenshot-at "571:${RV_SPIKE_UI_WORK_DIR}/middle.png"
            --screenshot-at "696:${RV_SPIKE_UI_WORK_DIR}/resized_1280x720.png"
            --screenshot-at "868:${RV_SPIKE_UI_WORK_DIR}/last_chip.png"
            --screenshot-at "1022:${RV_SPIKE_UI_WORK_DIR}/middle_list_focused.png"
            --screenshot-at "1030:${RV_SPIKE_UI_WORK_DIR}/text_scale_1.2.png"
            --screenshot-at "1100:${RV_SPIKE_UI_WORK_DIR}/end.png"
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_test(NAME gpu.spike_ui.input_script
    COMMAND spike_ui --smoke --frames 120 --hidden --no-vsync --fixed-dt 0.016667
            --input-script "${PROJECT_SOURCE_DIR}/tests/input_scripts/quick_catalog_select.json"
            --expect-selection 99998
            --data-dir "${RV_SPIKE_UI_WORK_DIR}/input_script_data"
            --report "${RV_SPIKE_UI_WORK_DIR}/input_script_report.json"
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/user_bindings.json"
    "${RV_SPIKE_UI_WORK_DIR}/user_bindings_data/config/bindings.json" COPYONLY)
add_test(NAME gpu.spike_ui.user_bindings
    COMMAND spike_ui --smoke --frames 80 --hidden --no-vsync --fixed-dt 0.016667 --scenario user-bindings
            --data-dir "${RV_SPIKE_UI_WORK_DIR}/user_bindings_data"
            --report "${RV_SPIKE_UI_WORK_DIR}/user_bindings_report.json"
    WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
# Skipped, like the smoke tests, where no OpenGL 4.5 core profile context can be created: AppHost logs this line on the
# console, then exits with 2.
set_tests_properties(gpu.spike_ui.scenario gpu.spike_ui.input_script gpu.spike_ui.user_bindings PROPERTIES
    LABELS gpu TIMEOUT 600 RUN_SERIAL TRUE SKIP_REGULAR_EXPRESSION "requires OpenGL 4\\.5 \\(core profile\\)")
