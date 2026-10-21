# rv_catalog_db: the SQLite store of rv_catalog's ports (ADR-0008 §5), GPU-free: connections (one writer, read-only
# readers), migrations with backups, the index (idx_*) and state (rv_*) tables, the catalog store, the queries with FTS5
# search and paging, the play session rows. SQLite stays private: its types never appear in the public headers. The
# schema areas are private sources, one per owning step (src/schema.hpp). Since I3 the room's bulk snapshot query
# (src/room_query.cpp, a skeleton until I3 step 2).

add_library(rv_catalog_db STATIC
    src/catalog_query.cpp
    src/catalog_stamp.cpp
    src/catalog_store.cpp
    src/database.cpp
    src/migrator.cpp
    src/room_query.cpp
    src/schema_identity.cpp
    src/schema_index.cpp
    src/schema_launch.cpp
    src/schema_search.cpp
    src/session_store.cpp
)
add_library(rv::catalog_db ALIAS rv_catalog_db)
rv_set_target_defaults(rv_catalog_db)
target_include_directories(rv_catalog_db PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_catalog_db PUBLIC rv_catalog PRIVATE unofficial::sqlite3::sqlite3)
rv_forbid_link(rv_catalog_db EXTRA ${RV_PUGIXML_LIBRARIES} rv_esde)
