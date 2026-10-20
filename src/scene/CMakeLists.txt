# rv_scene: camera, first-person and orbit controllers and viewpoints, lights, render items and batching plan, scene
# descriptions, scene content (geometry, models) and collision, debug lines; since I3 (skeletons naming their steps)
# the eased pose animator and the clusters of static instance groups; since I4 step 3 the scene's areas and the lights
# a view packs in the area that holds the eye. GPU-free: the renderer consumes its plain data.

add_library(rv_scene STATIC
    src/batch_planner.cpp
    src/camera.cpp
    src/collision.cpp
    src/debug_lines.cpp
    src/first_person_controller.cpp
    src/instance_clusters.cpp
    src/light.cpp
    src/orbit_controller.cpp
    src/pose_animator.cpp
    src/scene_areas.cpp
    src/scene_description.cpp
    src/scene_geometry.cpp
    src/viewpoint.cpp
)
add_library(rv::scene ALIAS rv_scene)
rv_set_target_defaults(rv_scene)
target_include_directories(rv_scene PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_scene PUBLIC rv_core rv_geometry rv_input rv_assets)
rv_forbid_link(rv_scene)
