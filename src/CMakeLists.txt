# First-party modules, in dependency order. See docs/architecture.md for the layering rules.

# GPU-free layer (unit-testable without a window or OpenGL context)
add_subdirectory(core)
add_subdirectory(geometry)
add_subdirectory(input)
add_subdirectory(assets)
add_subdirectory(scene)
add_subdirectory(media)    # video decoding with FFmpeg (I1 spike c)
add_subdirectory(audio)    # the clips' sound: the mixer and its output with miniaudio (I5 step 5, ADR-0016 §9)
add_subdirectory(launcher) # emulator launch policy (I1 spike f)
# The ES-DE catalog of milestone M1 (I2, ADR-0008): the domain and its ports, the ES-DE adapter (pugixml private), the
# SQLite store (SQLite private) and the Windows services without SDL (system probe, process runner).
add_subdirectory(catalog)
add_subdirectory(esde)
add_subdirectory(catalog_db)
add_subdirectory(platform_win)
# What the app and retroverse-cli share of a library (I7 step 2, ADR-0018 §13).
add_subdirectory(library)
# The world generator of milestone M2 (I3, ADR-0010): plans computed from the catalog's snapshot, GPU-free.
add_subdirectory(world)

# Platform and rendering layer
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(ui)       # RmlUi product UI; its GPU-free model always (I1 spike d)
# Developer overlay: its GPU-free model always, the Dear ImGui overlay itself only with RV_DEV_UI.
add_subdirectory(devui)
# VR through OpenXR (I10, ADR-0017): its GPU-free model always, the OpenXR runtime only with RV_ENABLE_XR.
add_subdirectory(xr)

# Composition root
add_subdirectory(app)

add_executable(retroverse main.cpp)
rv_set_target_defaults(retroverse)
rv_enable_long_paths(retroverse)
rv_set_windows_icon(retroverse)
target_link_libraries(retroverse PRIVATE rv_app)
rv_deploy_assets(retroverse)
