# rv_assets: CPU-side asset loading (images, glTF models, prefab manifests, material presets, shader sources,
# procedural test patterns). GPU-free: produces plain data consumed by the renderer.

add_library(rv_assets STATIC
    src/gltf_geometry.cpp
    src/gltf_loader.cpp
    src/image_decode.cpp
    src/image_encode.cpp
    src/image_io.cpp
    src/json_util.cpp
    src/material_library.cpp
    src/path_util.cpp
    src/prefab_manifest.cpp
    src/shader_source.cpp
    src/test_patterns.cpp
)
add_library(rv::assets ALIAS rv_assets)
rv_set_target_defaults(rv_assets)
target_include_directories(rv_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rv_assets SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(rv_assets PUBLIC rv_core rv_geometry PRIVATE fastgltf::fastgltf)
rv_forbid_link(rv_assets)
