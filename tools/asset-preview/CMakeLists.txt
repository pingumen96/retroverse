# retroverse-asset-preview (style guide §19.3, plan 5.7): previews a glTF model, a prefab manifest or a procedural
# prefab with the renderer, scene instantiation and developer overlay of rv_app.

# GPU-free core, built in every configuration so that rv_tests covers it (tests/unit/preview): the command line and
# its targets, the subject loaded from a target (model, manifest checks, prefab), its scene, screens, statistics,
# overlay lines and labels, the orbit controls and the CRT-like fixture of --write-fixture. No SDL, OpenGL or ImGui.
add_library(rv_preview_core STATIC
    src/asset_report.cpp
    src/crt_fixture.cpp
    src/preview_controls.cpp
    src/preview_options.cpp
    src/preview_overlay.cpp
    src/preview_scene.cpp
    src/preview_subject.cpp
    src/view_mode.cpp
)
add_library(rv::preview_core ALIAS rv_preview_core)
rv_set_target_defaults(rv_preview_core)
target_include_directories(rv_preview_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_preview_core PUBLIC rv_core rv_geometry rv_input rv_assets rv_scene rv_render_uniforms
                                             rv_app_cli)
rv_forbid_link(rv_preview_core)

if(NOT RV_DEV_UI)
    message(STATUS "retroverse-asset-preview needs RV_DEV_UI=ON (the Dear ImGui developer UI); skipping it")
    return()
endif()

add_executable(retroverse-asset-preview
    src/main.cpp
    src/preview_app.cpp
    src/preview_panels.cpp
)
rv_set_target_defaults(retroverse-asset-preview)
target_link_libraries(retroverse-asset-preview PRIVATE rv_preview_core rv_app rv_devui)
rv_deploy_assets(retroverse-asset-preview)
