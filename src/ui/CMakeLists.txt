# Product UI (I1 spike d, ADR-0005; I2 step 6).
#   rv_ui_model  GPU-free policy of the product UI, always built and unit-tested by rv_tests (tests/unit/ui): UI
#                commands from the UI actions of the action map (stick hysteresis, repeat timing), focus regions and the
#                focus history for Back, the paged catalog source (CatalogSource, InMemoryCatalogSource) and the quick
#                catalog's view model, a window over it; timing percentiles of the UI pass; since I3 (skeletons naming
#                their steps) the models of the world's card and of the media viewer; since I4 step 7a the directory's
#                (the building's floors and rooms, the favourites and recent games, their card); since I4b step 6 the
#                impact preview of a regeneration or an undo (RegeneratePreviewModel); since I7 step 1 the menu and the
#                settings screen (SystemMenuModel, SettingsModel); since I7 step 2 the wizard and the folder browser
#                (WizardModel, FolderBrowserModel).
#   rv_ui        RmlUi 6.3 on SDL3 and OpenGL 3: RmlUi's lifetime and interfaces (UiSystem), one context per output
#                (UiView: input, update, rendering into the window and screenshots, CPU and GPU times), the quick
#                catalog screen (QuickCatalogScreen, a fixed pool of rows), since I3 (skeletons) the world's card,
#                the hover label, the media viewer and Informazioni, since I4 step 7a the directory (DirectoryScreen),
#                since I4b step 6 the regeneration's preview (RegeneratePreviewScreen), since I7 step 1 the menu and the
#                settings screen (SystemMenuScreen, SettingsScreen), since I7 step 2 the wizard (WizardScreen);
#                the RmlUi backends are vendored in third_party/rmlui_backends on our glad loader (target
#                rv_rmlui_backends, used by rv_ui only).
#                It never links rv_catalog: rv_app adapts the catalog to CatalogSource.

add_library(rv_ui_model STATIC
    src/catalog_source.cpp
    src/directory_model.cpp
    src/filter_count_log.cpp
    src/focus_history.cpp
    src/folder_browser_model.cpp
    src/focus_regions.cpp
    src/game_card_model.cpp
    src/game_info_model.cpp
    src/in_memory_catalog_source.cpp
    src/media_viewer_model.cpp
    src/nav_input.cpp
    src/quick_catalog_model.cpp
    src/regenerate_preview_model.cpp
    src/settings_model.cpp
    src/system_menu_model.cpp
    src/timing_samples.cpp
    src/ui_command.cpp
    src/wizard_model.cpp
)
add_library(rv::ui_model ALIAS rv_ui_model)
rv_set_target_defaults(rv_ui_model)
target_include_directories(rv_ui_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_ui_model PUBLIC rv_core rv_input)
rv_forbid_link(rv_ui_model)

add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/rmlui_backends" "${CMAKE_CURRENT_BINARY_DIR}/rmlui_backends")

add_library(rv_ui STATIC
    src/directory_screen.cpp
    src/game_card_screen.cpp
    src/game_info_screen.cpp
    src/hover_label_screen.cpp
    src/media_viewer_screen.cpp
    src/quick_catalog_screen.cpp
    src/regenerate_preview_screen.cpp
    src/settings_screen.cpp
    src/system_menu_screen.cpp
    src/ui_system.cpp
    src/ui_view.cpp
    src/wizard_screen.cpp
)
add_library(rv::ui ALIAS rv_ui)
rv_set_target_defaults(rv_ui)
target_include_directories(rv_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_ui
    PUBLIC rv_ui_model rv_core rv_input rv_platform rv_render RmlUi::Core
    PRIVATE rv_rmlui_backends rv_devui_model SDL3::SDL3)
