# rv_render: OpenGL 4.5 core renderer (device, shaders, buffers, textures, scene renderer, debug drawing).
# Receives the GL function loader as a function pointer and never includes SDL.

add_library(rv_render STATIC
    src/render_module.cpp
)
add_library(rv::render ALIAS rv_render)
rv_set_target_defaults(rv_render)
target_include_directories(rv_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_render PUBLIC rv_core rv_geometry rv_assets rv_scene rv::glad)
