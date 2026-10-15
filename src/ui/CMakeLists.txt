# Product UI (I1 spike d).
#   rv_ui_model  GPU-free policy of the product UI (navigation and focus from rv::input actions), always built and
#                unit-tested by rv_tests.
#   rv_ui        RmlUi on SDL3 and OpenGL 3: context, documents, fonts, input and rendering, with the RmlUi backends
#                vendored in third_party/rmlui_backends on our glad loader.

add_library(rv_ui_model STATIC
    src/ui_model_module.cpp
)
add_library(rv::ui_model ALIAS rv_ui_model)
rv_set_target_defaults(rv_ui_model)
target_include_directories(rv_ui_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_ui_model PUBLIC rv_core rv_input)
rv_forbid_link(rv_ui_model)

add_library(rv_ui STATIC
    src/ui_module.cpp
)
add_library(rv::ui ALIAS rv_ui)
rv_set_target_defaults(rv_ui)
target_include_directories(rv_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_ui PUBLIC rv_ui_model rv_core rv_platform rv_render RmlUi::RmlUi)
