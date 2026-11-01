# rv_library (I7 step 2, ADR-0018 §13): what the app and retroverse-cli share of a library, above the ES-DE adapter, the
# catalog database and the Windows services: the one launch library (LaunchLibrary: ES-DE's settings, systems and find
# rules, the emulator profiles, the machine), making a library (draftLibrary, createLibrary: library.json) and reading
# what it holds without writing anything (previewLibrary: `import --dry-run`'s reading, the wizard's page 3). GPU-free;
# read-only towards ES-DE: library.json is written under the user data directory.

add_library(rv_library STATIC
    src/folder_listing.cpp
    src/launch_library.cpp
    src/library_creation.cpp
    src/library_preview.cpp
)
add_library(rv::library ALIAS rv_library)
rv_set_target_defaults(rv_library)
target_include_directories(rv_library PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_library PUBLIC rv_core rv_launcher rv_catalog rv_esde rv_catalog_db rv_platform_win)
rv_forbid_link(rv_library EXTRA ${RV_RMLUI_LIBRARIES} rv_world)
