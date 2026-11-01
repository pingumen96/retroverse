# rv_app: composition root (application host, main loop and its idle mode, input pipeline, free roam, scene
# instantiation, the model viewer, screenshots, the product UI and its launches). Links every module.

# GPU-free part of the application layer: command line options, scene selection, screenshot naming, the exit-code
# policy, the Back action policy, the input contexts of the world and the product UI, when a run needs the developer
# overlay and which screenshots show it, the verdicts of the rendering self-checks, the scene content plan (distinct
# meshes, materials and picture instances, render items), the model viewer scene, the debug line selection, the frame
# statistics log, the run report (--run-report), and the launch shell of the quick catalog: the mock catalog
# (--mock-catalog), a launch from Avvia to the return around a LaunchSession (LaunchFlow) and its outcome as the product
# UI tells it; since I3 (skeletons naming their steps) the frame timing of the run report, the world's interaction
# states, what the hero CRT plays, the cover loader's order, a collection item's layers through the derived-texture
# cache and the world notice's keys (step 10b), the room's items kept for the persisted start (step 10c), the room's
# side of the hover, the pull-out and Focus and the hover label's keys (step 13a), the held item's pose (step 13b); since
# I4 step 7b the elevator ride's state machine and the directory's data and command routing; since I5 step 1 the
# screens' playlists and video.json; since I5 step 2 the decoder scheduler and the process priority's policy; since I5
# step 5 the sound's policy (the output, the audible screen, its gain and pan, the duck); since I5 step 4 the picture's
# aspect on its screen and the arcade cabinets that play.
# Kept apart from rv_app so that rv_tests can unit-test it without SDL or OpenGL, and the GPU smoke tests can read the
# reports of the runs they start.
add_library(rv_app_cli STATIC
    src/audio_policy.cpp
    src/back_action.cpp
    src/cabinet_screens.cpp
    src/collection_layers.cpp
    src/command_line.cpp
    src/cover_requests.cpp
    src/crt_looks.cpp
    src/debug_line_selection.cpp
    src/directory_data.cpp
    src/elevator_ride.cpp
    src/exit_code.cpp
    src/frame_stats_log.cpp
    src/frame_timing.cpp
    src/binding_rules.cpp
    src/controller_watch.cpp
    src/input_context.cpp
    src/launch_flow.cpp
    src/launch_outcome.cpp
    src/mock_catalog.cpp
    src/model_viewer.cpp
    src/overlay_policy.cpp
    src/picture_bounds.cpp
    src/player_settings.cpp
    src/playlist.cpp
    src/process_priority.cpp
    src/room_items.cpp
    src/run_report.cpp
    src/scene_content_plan.cpp
    src/screen_aspect.cpp
    src/screen_policy.cpp
    src/screen_room.cpp
    src/screen_scheduler.cpp
    src/screenshot_name.cpp
    src/self_check.cpp
    src/setting_rows.cpp
    src/settings_store.cpp
    src/video_settings.cpp
    src/world_focus.cpp
    src/world_inspect.cpp
    src/world_interaction.cpp
    src/world_edit_interaction.cpp
    src/world_edit_room.cpp
)
add_library(rv::app_cli ALIAS rv_app_cli)
rv_set_target_defaults(rv_app_cli)
target_include_directories(rv_app_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# rv_world (I3): the world's plans, picking and the interaction states of the generated room (ADR-0010, ADR-0012).
# rv_audio_model (I5 step 5): the outputs' names and the spatial rules, without the engine or miniaudio.
target_link_libraries(rv_app_cli PUBLIC
    rv_core rv_geometry rv_input rv_assets rv_scene rv_launcher rv_ui_model rv_render_uniforms rv_world rv_audio_model)
rv_forbid_link(rv_app_cli)

# GPU-free part of the application layer over the ES-DE catalog (I2 step 13): the catalog worker (the database's writer
# and the queries on its own thread), the quick catalog's source over it, the launches of the catalog resolved with the
# library's configuration, their play sessions, and the AppCatalog the product UI drives (the ES-DE catalog, or the mock
# catalog of the tests); since I3 the catalog's refresh on its worker, the room's snapshot and the room's making up to
# its commit (world_build: the jobs on the worker pool and the main thread's flow, step 10b), the persisted start's
# reconciliation and the hero CRT's games of the room (world_reconcile, room_screen: step 10c), the live update's decision
# (world_live_plan: step 15b), what the media viewer and Informazioni show of a game (game_screens: step 14b); since I4b
# the editor's draft (world_edit_draft: step 3) and the regenerations' and the undo's jobs (world_regenerate_job: step
# 6); since I5 step 8's review the screens' verdicts (screen_verdicts: I5 step 7, over rv_media's verdicts file, unit
# tested); since I7 step 2 the wizard's flow (wizard_flow). Kept apart from rv_app_cli, which the GPU smoke tests link for the run report alone.
add_library(rv_app_catalog STATIC
    src/app_catalog.cpp
    src/catalog_database_source.cpp
    src/catalog_worker.cpp
    src/game_screens.cpp
    src/hub_showcase.cpp
    src/library_refresh.cpp
    src/room_screen.cpp
    src/room_snapshot.cpp
    src/screen_verdicts.cpp
    src/wizard_flow.cpp
    src/world_build.cpp
    src/world_live_plan.cpp
    src/world_reconcile.cpp
    src/world_edit_draft.cpp
    src/world_regenerate_job.cpp
)
add_library(rv::app_catalog ALIAS rv_app_catalog)
rv_set_target_defaults(rv_app_catalog)
target_include_directories(rv_app_catalog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_app_catalog PUBLIC
    rv_app_cli rv_core rv_launcher rv_ui_model rv_catalog rv_esde rv_catalog_db rv_platform_win rv_media rv_library)
rv_forbid_link(rv_app_catalog)

add_library(rv_app STATIC
    src/app.cpp
    src/app_host.cpp
    src/app_input.cpp
    src/app_logic.cpp
    src/bloom_self_check.cpp
    src/collection_instance.cpp
    src/cover_loader.cpp
    src/elevator_prompt_panel.cpp
    src/free_roam.cpp
    src/fxaa_self_check.cpp
    src/game_screens_panel.cpp
    src/game_session_panel.cpp
    src/hero_textures.cpp
    src/product_ui.cpp
    src/retroverse_app.cpp
    src/scene_graphics.cpp
    src/scene_instance.cpp
    src/screen_service.cpp
    src/self_check_canvas.cpp
    src/srgb_self_check.cpp
    src/world_cabinets.cpp
    src/world_card.cpp
    src/world_hover.cpp
    src/world_inspection.cpp
    src/world_label_panel.cpp
    src/world_launch.cpp
    src/world_live_update.cpp
    src/world_notice_panel.cpp
    src/world_ride.cpp
    src/world_session.cpp
    src/world_showcase.cpp
    src/world_start.cpp
    src/edit_bar_panel.cpp
    src/world_edit.cpp
    src/world_regenerate.cpp
    src/xr_game.cpp
    src/xr_movement.cpp
    src/xr_session_driver.cpp
    src/xr_ui.cpp
)
add_library(rv::app ALIAS rv_app)
rv_set_target_defaults(rv_app)
target_include_directories(rv_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# rv_ui: the product UI (RmlUi); rv_launcher and rv_platform_win: launches from the quick catalog (plans, sessions, the
# Win32 process runner and system probe); rv_devui_model: the mouse's hand-over between the scene and the layers over it
# (GPU-free, in every build); rv_media: the hero CRT's video decoder (I3, ADR-0012 §7); rv_world: the generated room
# (I3, ADR-0010); rv_audio: the clips' sound (I5 step 5, ADR-0016 §9), connected to rv_media's decoders here; rv_xr:
# the VR mode (I10, ADR-0017), and rv_platform_win_capture its game captured onto the CRT (I10 step 6).
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
    rv_devui_model
    rv_media
    rv_audio
    rv_world
    rv_xr
    rv_platform_win_capture)

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
