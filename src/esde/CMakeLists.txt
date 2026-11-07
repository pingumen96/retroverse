# rv_esde: the ES-DE adapter of rv_catalog's ports (ADR-0008), GPU-free and read-only towards ES-DE: discovery of its
# home and install folders, the settings Retroverse uses, systems and find rules, command templates, gamelists, the ROM
# scan with disc groups and MAME sets, the library source (EsdeLibrarySource), the media layout and the translation of
# ES-DE commands into launch plans; since I11 the systems' identity from ES-DE's themes and identity.json (ADR-0019).
# pugixml stays private, behind the module's XML loader (src/xml.hpp). Skeleton: each file names the I2 step that fills
# it.

add_library(rv_esde STATIC
    src/change_scope.cpp
    src/command_template.cpp
    src/disc_groups.cpp
    src/discovery.cpp
    src/find_rules.cpp
    src/gamelist.cpp
    src/identity.cpp
    src/launch_translation.cpp
    src/library_source.cpp
    src/mame_sets.cpp
    src/media_resolver.cpp
    src/rom_scanner.cpp
    src/settings.cpp
    src/systems.cpp
    src/themes.cpp
    src/xml.cpp
)
add_library(rv::esde ALIAS rv_esde)
rv_set_target_defaults(rv_esde)
target_include_directories(rv_esde PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_esde PUBLIC rv_catalog PRIVATE pugixml::pugixml)
rv_forbid_link(rv_esde EXTRA ${RV_SQLITE_LIBRARIES} rv_catalog_db)
