# rv_ui_probe: the GPU smoke tests' driver of the product UI's screens from fixed models (I4 step 7a): the directory
# (rv::ui::DirectoryScreen on assets/ui/directory.rml) over the test room with the host and renderer of rv_app, from the
# fixed model of directory_fixture.hpp, driven by an input script's UI actions through the action map and NavInput, its
# checkpoints recorded (--probe-report) and screenshotted (--probe-shots), which tests/smoke/test_directory_panel.cpp
# checks. directory_fixture.hpp, GPU-free, is shared with that test and with tests/unit/ui/test_directory.cpp. Built with
# the tests; it opens a window (hidden in every test run) like retroverse.exe.

add_executable(rv_ui_probe main.cpp probe_app.cpp)
rv_set_target_defaults(rv_ui_probe)
target_include_directories(rv_ui_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rv_ui_probe PRIVATE rv_app rv_ui nlohmann_json::nlohmann_json)
# Release runs read the assets deployed next to the executables.
rv_deploy_assets(rv_ui_probe)
