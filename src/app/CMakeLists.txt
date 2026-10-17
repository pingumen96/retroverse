# rv_app: composition root (application host, main loop and its idle mode, input pipeline, free roam, scene
# instantiation, the model viewer, screenshots, the product UI and its launches). Links every module.

# GPU-free part of the application layer: command line options, scene selection, screenshot naming, the exit-code
# policy, the Back action policy, the input contexts of the world and the product UI, when a run needs the developer
# overlay and which screenshots show it, the verdicts of the rendering self-checks, the scene content plan (distinct
# meshes, materials and picture instances, render items), the model viewer scene, the debug line selection, the frame
# statistics log, the run report (--run-report), and the launch shell of the quick catalog: the mock catalog
# (--mock-catalog), a launch from Avvia to the return around a LaunchSession (LaunchFlow) and its outcome as the product
# UI tells it. Kept apart from rv_app so that rv_tests can unit-test it without SDL or OpenGL, and the GPU smoke tests
# can read the reports of the runs they start.
add_library(rv_app_cli STATIC
    src/back_action.cpp
    src/command_line.cpp
    src/debug_line_selection.cpp
    src/exit_code.cpp
    src/frame_stats_log.cpp
    src/input_context.cpp
    src/launch_flow.cpp
    src/launch_outcome.cpp
    src/mock_catalog.cpp
    src/model_viewer.cpp
    src/overlay_policy.cpp
    src/run_report.cpp
    src/scene_content_plan.cpp
    src/screenshot_name.cpp
    src/self_check.cpp
)
add_library(rv::app_cli ALIAS rv_app_cli)
rv_set_target_defaults(rv_app_cli)
target_include_directories(rv_app_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app_cli PUBLIC
    rv_core rv_geometry rv_input rv_assets rv_scene rv_launcher rv_ui_model rv_render_uniforms)
rv_forbid_link(rv_app_cli)

# GPU-free part of the application layer over the ES-DE catalog (I2 step 13): the catalog worker (the database's writer
# and the queries on its own thread), the quick catalog's source over it, the launches of the catalog resolved with the
# library's configuration, their play sessions, and the AppCatalog the product UI drives (the ES-DE catalog, or the mock
# catalog of the tests). Kept apart from rv_app_cli, which the GPU smoke tests link for the run report alone.
add_library(rv_app_catalog STATIC
    src/app_catalog.cpp
    src/catalog_database_source.cpp
    src/catalog_launch_library.cpp
    src/catalog_worker.cpp
)
add_library(rv::app_catalog ALIAS rv_app_catalog)
rv_set_target_defaults(rv_app_catalog)
target_include_directories(rv_app_catalog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app_catalog PUBLIC
    rv_app_cli rv_core rv_launcher rv_ui_model rv_catalog rv_esde rv_catalog_db rv_platform_win)
rv_forbid_link(rv_app_catalog)

add_library(rv_app STATIC
    src/app.cpp
    src/app_host.cpp
    src/app_input.cpp
    src/app_logic.cpp
    src/bloom_self_check.cpp
    src/free_roam.cpp
    src/fxaa_self_check.cpp
    src/game_session_panel.cpp
    src/product_ui.cpp
    src/retroverse_app.cpp
    src/scene_graphics.cpp
    src/scene_instance.cpp
    src/self_check_canvas.cpp
    src/srgb_self_check.cpp
)
add_library(rv::app ALIAS rv_app)
rv_set_target_defaults(rv_app)
target_include_directories(rv_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# rv_ui: the product UI (RmlUi); rv_launcher and rv_platform_win: launches from the quick catalog (plans, sessions, the
# Win32 process runner and system probe); rv_devui_model: the mouse's hand-over between the scene and the layers over it
# (GPU-free, in every build).
target_link_libraries(rv_app PUBLIC
    rv_app_cli
    rv_app_catalog
    rv_core
    rv_geometry
    rv_input
    rv_assets
    rv_scene
    rv_launcher
    rv_platform_win
    rv_platform
    rv_render
    rv_ui
    rv_devui_model)

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
