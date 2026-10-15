# rv_devui: Dear ImGui developer overlay (developer builds only, RV_DEV_UI): the DevUi host (ImGui context, SDL3 and
# OpenGL3 backends, event routing, dockspace) and its panels (statistics, camera, lights, materials, rendering, log,
# scene). Never part of the product UI.

# GPU-free part, always built so that rv_tests covers it in every configuration: the frame time history, the input
# routing and mouse capture hand-over policies, the overlay's copy of the log. No SDL, OpenGL or ImGui.
add_library(rv_devui_model STATIC
    src/frame_time_history.cpp
    src/log_view.cpp
    src/overlay_input.cpp
)
add_library(rv::devui_model ALIAS rv_devui_model)
rv_set_target_defaults(rv_devui_model)
target_include_directories(rv_devui_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_devui_model PUBLIC rv_core)
rv_forbid_link(rv_devui_model)

if(NOT RV_DEV_UI)
    return()
endif()

add_library(rv_devui STATIC
    src/dev_ui.cpp
    src/panel_camera.cpp
    src/panel_lights.cpp
    src/panel_log.cpp
    src/panel_materials.cpp
    src/panel_rendering.cpp
    src/panel_scene.cpp
    src/panel_stats.cpp
    src/widgets.cpp
)
add_library(rv::devui ALIAS rv_devui)
rv_set_target_defaults(rv_devui)
target_include_directories(rv_devui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_devui PUBLIC rv_devui_model rv_core rv_input rv_scene rv_platform rv_render imgui::imgui)
