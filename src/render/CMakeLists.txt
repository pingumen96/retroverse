# rv_render: OpenGL 4.5 core renderer (device, shaders, buffers, textures, mesh and material registries, glTF models,
# the batched and instanced scene renderer, HDR view targets, bloom and the post pass, debug lines, framebuffers).
# Receives the GL function loader as a function pointer and never includes SDL.

# GPU-free policy pieces of the renderer, so rv_tests can unit-test them without a context. Their headers include no
# OpenGL header.
#   rv_render_debug_filter  which GL debug messages are logged, dropped or de-duplicated
#   rv_render_uniforms      std140 uniform block mirrors (uniform_blocks.hpp) and their packing from scene lights,
#                           the environment and materials; the post-processing settings, their block and the bloom
#                           chain's level sizes (post_settings.hpp); the CPU mirror of the display transform
#                           (tonemap.hpp); the renderer's frame statistics (frame_stats.hpp, header only)
add_library(rv_render_debug_filter STATIC
    src/debug_message_filter.cpp
)
add_library(rv::render_debug_filter ALIAS rv_render_debug_filter)
rv_set_target_defaults(rv_render_debug_filter)
target_include_directories(rv_render_debug_filter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_render_debug_filter PUBLIC rv_core)
rv_forbid_link(rv_render_debug_filter)

add_library(rv_render_uniforms STATIC
    src/light_packing.cpp
    src/material_uniforms.cpp
    src/post_settings.cpp
    src/tonemap.cpp
)
add_library(rv::render_uniforms ALIAS rv_render_uniforms)
rv_set_target_defaults(rv_render_uniforms)
target_include_directories(rv_render_uniforms PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_render_uniforms PUBLIC rv_core rv_assets rv_scene)
rv_forbid_link(rv_render_uniforms)

add_library(rv_render STATIC
    src/buffer.cpp
    src/debug_draw.cpp
    src/framebuffer.cpp
    src/gl_device.cpp
    src/gl_object_label.cpp
    src/gpu_mesh.cpp
    src/gpu_model.cpp
    src/gpu_timer.cpp
    src/instance_buffer.cpp
    src/material_registry.cpp
    src/mesh_registry.cpp
    src/post_processor.cpp
    src/render_state_cache.cpp
    src/sampler.cpp
    src/scene_renderer.cpp
    src/shader_library.cpp
    src/shader_program.cpp
    src/texture.cpp
    src/texture_cache.cpp
    src/texture_defaults.cpp
    src/vertex_array.cpp
    src/view_targets.cpp
)
add_library(rv::render ALIAS rv_render)
rv_set_target_defaults(rv_render)
target_include_directories(rv_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_render PUBLIC
    rv_core
    rv_geometry
    rv_assets
    rv_scene
    rv_render_debug_filter
    rv_render_uniforms
    rv::glad)
