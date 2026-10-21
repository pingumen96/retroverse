# rv_catalog: the catalog domain of milestone M1 (ADR-0008), GPU-free and storage-free: libraries, systems, games and
# their entry states, media kinds, disc groups, metadata and its normalizers, library snapshots, change sets, play
# sessions, ids and lookup keys; one header per port (ILibrarySource, ICatalogStore, ICatalogQuery with the paging
# contract, IMediaResolver; the system probe's port is rv_core's ISystemProbe); the services that only see ports:
# coverage, launch resolution, import and reconciliation, fingerprints. Never reaches SQLite, pugixml or the Windows-only
# targets (rv_esde and rv_catalog_db implement its ports). Skeleton: each file names the I2 step that fills it; ports
# and plain types are header-only.

add_library(rv_catalog STATIC
    src/coverage.cpp
    src/fingerprint.cpp
    src/identity.cpp
    src/ids.cpp
    src/import_service.cpp
    src/launch_resolution.cpp
    src/lookup_key.cpp
    src/metadata.cpp
    src/recent_games.cpp
    src/reconciliation.cpp
)
add_library(rv::catalog ALIAS rv_catalog)
rv_set_target_defaults(rv_catalog)
target_include_directories(rv_catalog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_catalog PUBLIC rv_core rv_launcher)
rv_forbid_link(rv_catalog EXTRA
    ${RV_SQLITE_LIBRARIES} ${RV_PUGIXML_LIBRARIES} rv_platform_win rv_platform rv_esde rv_catalog_db)
