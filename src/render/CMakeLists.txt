# rv_render: OpenGL 4.5 core renderer (device, shaders, buffers, textures, scene renderer, debug drawing).
# Receives the GL function loader as a function pointer and never includes SDL.

# GPU-free policy pieces of the renderer (GL debug message filtering), so rv_tests can unit-test them without a
# context. Their headers include no OpenGL header.
add_library(rv_render_debug_filter STATIC
    src/debug_message_filter.cpp
)
add_library(rv::render_debug_filter ALIAS rv_render_debug_filter)
rv_set_target_defaults(rv_render_debug_filter)
target_include_directories(rv_render_debug_filter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_render_debug_filter PUBLIC rv_core)
rv_forbid_link(rv_render_debug_filter)

add_library(rv_render STATIC
    src/buffer.cpp
    src/gl_device.cpp
    src/gl_object_label.cpp
    src/render_state_cache.cpp
    src/shader_program.cpp
    src/vertex_array.cpp
)
add_library(rv::render ALIAS rv_render)
rv_set_target_defaults(rv_render)
target_include_directories(rv_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_render PUBLIC rv_core rv_geometry rv_assets rv_scene rv_render_debug_filter rv::glad)
