# rv_geometry: CPU mesh data, procedural mesh builder and procedural prefabs. GPU-free.

add_library(rv_geometry STATIC
    src/mesh_builder.cpp
    src/mesh_data.cpp
    src/mesh_detail.cpp
    src/prefab_assembly.cpp
    src/prefab_cartridge.cpp
    src/prefab_console_cartridge.cpp
    src/prefab_crt.cpp
    src/prefab_factory.cpp
    src/prefab_floor_lamp.cpp
    src/prefab_poster_frame.cpp
    src/prefab_room_shell.cpp
    src/prefab_rug.cpp
    src/prefab_shelf.cpp
    src/prefab_tv_cabinet.cpp
    src/socket.cpp
)
add_library(rv::geometry ALIAS rv_geometry)
rv_set_target_defaults(rv_geometry)
target_include_directories(rv_geometry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_geometry PUBLIC rv_core)
rv_forbid_link(rv_geometry)
