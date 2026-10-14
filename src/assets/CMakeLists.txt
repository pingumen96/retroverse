# rv_assets: CPU-side asset loading (images, glTF models, prefab manifests, material presets, shader sources).
# GPU-free: produces plain data consumed by the renderer.

add_library(rv_assets STATIC
    src/assets_module.cpp
)
add_library(rv::assets ALIAS rv_assets)
rv_set_target_defaults(rv_assets)
target_include_directories(rv_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rv_assets SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(rv_assets PUBLIC rv_core rv_geometry PRIVATE fastgltf::fastgltf)
rv_forbid_link(rv_assets)
