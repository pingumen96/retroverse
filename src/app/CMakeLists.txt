# rv_app: composition root (application host, main loop, input pipeline, free roam, scene instantiation, the model
# viewer, screenshots). Links every module.

# GPU-free part of the application layer: command line options, scene selection, screenshot naming, the exit-code
# policy, the Back action policy, when a run needs the developer overlay and which screenshots show it, the verdicts of
# the rendering self-checks, the scene content plan (distinct meshes, materials and picture instances, render items),
# the model viewer scene, the debug line selection and the frame statistics log. Kept apart from rv_app so that
# rv_tests can unit-test it without SDL or OpenGL.
add_library(rv_app_cli STATIC
    src/back_action.cpp
    src/command_line.cpp
    src/debug_line_selection.cpp
    src/exit_code.cpp
    src/frame_stats_log.cpp
    src/model_viewer.cpp
    src/overlay_policy.cpp
    src/scene_content_plan.cpp
    src/screenshot_name.cpp
    src/self_check.cpp
)
add_library(rv::app_cli ALIAS rv_app_cli)
rv_set_target_defaults(rv_app_cli)
target_include_directories(rv_app_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app_cli PUBLIC rv_core rv_geometry rv_assets rv_scene rv_render_uniforms)
rv_forbid_link(rv_app_cli)

add_library(rv_app STATIC
    src/app.cpp
    src/app_host.cpp
    src/app_input.cpp
    src/app_logic.cpp
    src/free_roam.cpp
    src/retroverse_app.cpp
    src/scene_instance.cpp
    src/srgb_self_check.cpp
)
add_library(rv::app ALIAS rv_app)
rv_set_target_defaults(rv_app)
target_include_directories(rv_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app PUBLIC
    rv_app_cli
    rv_core
    rv_geometry
    rv_input
    rv_assets
    rv_scene
    rv_platform
    rv_render)

if(RV_DEV_UI)
    target_link_libraries(rv_app PUBLIC rv_devui)
    target_compile_definitions(rv_app PUBLIC RV_DEV_UI=1)
endif()
if(RV_GL_DEBUG)
    target_compile_definitions(rv_app PUBLIC RV_GL_DEBUG=1)
endif()
# Developer builds read assets from the source tree: reload shaders as soon as their files change.
if(RV_DEV_ASSET_ROOT)
    target_compile_definitions(rv_app PRIVATE RV_DEV_ASSET_ROOT=1)
endif()
