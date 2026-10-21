# rv_world: the world generator of milestone M2 (ADR-0010), GPU-free: the WorldPlan and its canonical JSON, the
# world's identifiers, configuration and data files (templates, platforms, media families, presets under
# assets/world), the room's selection from the catalog's snapshot, generation, validation, the incremental update,
# persistence and where-is, the plan's static part as a scene description and the collection's layout, picking and
# the D-pad steps (ADR-0012 §1, §2); since I4 the building (ADR-0013): its data, the floors' layout, halls and scenes.
# It computes plans and never creates a mesh, a texture or a GPU object. The files that are still skeletons name the I3
# step that fills them; plain types are header-only; data_reader is the strict JSON reading the plan and the data files
# share (I3 step 1a).

include("${CMAKE_CURRENT_SOURCE_DIR}/forbidden_links.cmake")

add_library(rv_world STATIC
    src/allocation.cpp
    src/capacity.cpp
    src/collection_layout.cpp
    src/data_reader.cpp
    src/fixture_layout.cpp
    src/building.cpp
    src/floor_layout.cpp
    src/floor_scene.cpp
    src/generation.cpp
    src/ids.cpp
    src/incremental.cpp
    src/layout_score.cpp
    src/lift.cpp
    src/media_family.cpp
    src/nav_grid.cpp
    src/overrides.cpp
    src/pick_index.cpp
    src/plan_json.cpp
    src/platform_manifest.cpp
    src/room_selection.cpp
    src/room_look.cpp
    src/room_template.cpp
    src/scene_builder.cpp
    src/seeds.cpp
    src/slot_navigation.cpp
    src/title_key.cpp
    src/validation.cpp
    src/where_is.cpp
    src/world_config.cpp
    src/world_plan.cpp
    src/world_store.cpp
)
add_library(rv::world ALIAS rv_world)
rv_set_target_defaults(rv_world)
target_include_directories(rv_world PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# rv_geometry: the prefabs' slot sockets; rv_assets: the manifests; rv_scene: the static part as a SceneDescription;
# rv_catalog: GameIds and the catalog's domain.
target_link_libraries(rv_world PUBLIC rv_core rv_geometry rv_assets rv_scene rv_catalog)
rv_forbid_link(rv_world EXTRA ${RV_WORLD_FORBIDDEN_LINKS})
