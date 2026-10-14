# rv_geometry: CPU mesh data, procedural mesh builder and procedural prefabs. GPU-free.

add_library(rv_geometry STATIC
    src/mesh_data.cpp
    src/socket.cpp
)
add_library(rv::geometry ALIAS rv_geometry)
rv_set_target_defaults(rv_geometry)
target_include_directories(rv_geometry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_geometry PUBLIC rv_core)
rv_forbid_link(rv_geometry)
