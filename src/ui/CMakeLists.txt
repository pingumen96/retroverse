# Product UI (I1 spike d, ADR-0005).
#   rv_ui_model  GPU-free policy of the product UI, always built and unit-tested by rv_tests (tests/unit/ui): UI
#                commands from the pad and the keyboard (stick hysteresis, repeat timing, bindings), focus regions and
#                the focus history for Back, the quick catalog's view model, timing percentiles of the UI pass.
#   rv_ui        RmlUi 6.3 on SDL3 and OpenGL 3: RmlUi's lifetime and interfaces (UiSystem), one context per output
#                (UiView: input, update, rendering into the window and screenshots, CPU and GPU times), the quick
#                catalog screen (QuickCatalogScreen); the RmlUi backends are vendored in third_party/rmlui_backends on
#                our glad loader (target rv_rmlui_backends, used by rv_ui only).

add_library(rv_ui_model STATIC
    src/focus_history.cpp
    src/focus_regions.cpp
    src/nav_input.cpp
    src/quick_catalog_model.cpp
    src/timing_samples.cpp
    src/ui_command.cpp
)
add_library(rv::ui_model ALIAS rv_ui_model)
rv_set_target_defaults(rv_ui_model)
target_include_directories(rv_ui_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_ui_model PUBLIC rv_core rv_input)
rv_forbid_link(rv_ui_model)

add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/rmlui_backends" "${CMAKE_CURRENT_BINARY_DIR}/rmlui_backends")

add_library(rv_ui STATIC
    src/quick_catalog_screen.cpp
    src/ui_system.cpp
    src/ui_view.cpp
)
add_library(rv::ui ALIAS rv_ui)
rv_set_target_defaults(rv_ui)
target_include_directories(rv_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_ui
    PUBLIC rv_ui_model rv_core rv_input rv_platform rv_render RmlUi::Core
    PRIVATE rv_rmlui_backends rv_devui_model SDL3::SDL3)
