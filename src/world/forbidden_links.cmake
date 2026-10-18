# What rv_world must never reach (ADR-0010 §1), besides the GPU libraries every GPU-free target is kept from
# (cmake/LayerRules.cmake: SDL3, glad, Dear ImGui): RmlUi, SQLite, pugixml, the video decoder and the ES-DE, database
# and Windows modules. The world computes plans from plain data; the app and retroverse-cli hand it the catalog's
# snapshot. Included by CMakeLists.txt beside it and by the layering test (tests/cmake/test_layer_rules.cmake), which
# configures a project that breaks each rule.

set(RV_WORLD_FORBIDDEN_LINKS
    ${RV_RMLUI_LIBRARIES}
    ${RV_SQLITE_LIBRARIES}
    ${RV_PUGIXML_LIBRARIES}
    rv_media
    rv_ffmpeg
    rv_esde
    rv_catalog_db
    rv_platform_win
    rv_platform)
