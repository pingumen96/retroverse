# rv_assets: CPU-side asset loading (images, glTF models, prefab manifests and, since I3 step 1b, their families and
# fallbacks, material presets, shader sources, procedural test patterns and surface textures) and the GLB writer;
# since I3 (skeletons naming step 5) the cover and
# label pipeline: fit rules, mips in linear light, BC1, text, generated labels and spines, the .rvtex files of the
# derived-texture cache. GPU-free: produces plain data consumed by the renderer.

add_library(rv_assets STATIC
    src/bc1.cpp
    src/collection_labels.cpp
    src/cover_fit.cpp
    src/crt_presets.cpp
    src/derived_texture_cache.cpp
    src/glb_writer.cpp
    src/gltf_geometry.cpp
    src/gltf_loader.cpp
    src/image_decode.cpp
    src/image_encode.cpp
    src/image_io.cpp
    src/image_orientation.cpp
    src/json_util.cpp
    src/material_library.cpp
    src/mip_chain.cpp
    src/path_util.cpp
    src/prefab_manifest.cpp
    src/prefab_resolver.cpp
    src/procedural_noise.cpp
    src/rvtex.cpp
    src/shader_source.cpp
    src/surface_canvas.cpp
    src/surface_finishes.cpp
    src/surface_rug.cpp
    src/surface_textures.cpp
    src/surface_wood.cpp
    src/test_patterns.cpp
    src/text_raster.cpp
)
add_library(rv::assets ALIAS rv_assets)
rv_set_target_defaults(rv_assets)
target_include_directories(rv_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rv_assets SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(rv_assets PUBLIC rv_core rv_geometry PRIVATE fastgltf::fastgltf)
rv_forbid_link(rv_assets)
