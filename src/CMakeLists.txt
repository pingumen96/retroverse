# First-party modules, in dependency order. See docs/architecture.md for the layering rules.

# GPU-free layer (unit-testable without a window or OpenGL context)
add_subdirectory(core)
add_subdirectory(geometry)
add_subdirectory(input)
add_subdirectory(assets)
add_subdirectory(scene)
add_subdirectory(media)    # video decoding with FFmpeg (I1 spike c)
add_subdirectory(launcher) # emulator launch policy (I1 spike f)

# Platform and rendering layer
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(ui)       # RmlUi product UI; its GPU-free model always (I1 spike d)
# Developer overlay: its GPU-free model always, the Dear ImGui overlay itself only with RV_DEV_UI.
add_subdirectory(devui)

# Composition root
add_subdirectory(app)

add_executable(retroverse main.cpp)
rv_set_target_defaults(retroverse)
target_link_libraries(retroverse PRIVATE rv_app)
rv_deploy_assets(retroverse)
