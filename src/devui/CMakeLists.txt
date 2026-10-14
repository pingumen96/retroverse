# rv_devui: Dear ImGui developer overlay (statistics, camera, lights, materials, log). Developer builds only
# (RV_DEV_UI); never part of the product UI.

add_library(rv_devui STATIC
    src/devui_module.cpp
)
add_library(rv::devui ALIAS rv_devui)
rv_set_target_defaults(rv_devui)
target_include_directories(rv_devui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_devui PUBLIC rv_core rv_platform rv_render imgui::imgui)
