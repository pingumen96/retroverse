# First-party modules, in dependency order. See docs/architecture.md for the layering rules.

# GPU-free layer (unit-testable without a window or OpenGL context)
add_subdirectory(core)
add_subdirectory(geometry)
add_subdirectory(input)
add_subdirectory(assets)
add_subdirectory(scene)

# Platform and rendering layer
add_subdirectory(platform)
add_subdirectory(render)
if(RV_DEV_UI)
    add_subdirectory(devui)
endif()

# Composition root
add_subdirectory(app)

add_executable(retroverse main.cpp)
rv_set_target_defaults(retroverse)
target_link_libraries(retroverse PRIVATE rv_app)
rv_deploy_assets(retroverse)
