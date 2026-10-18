// I3 step 0a: rv_world is linked into rv_tests and every planned public header of I3's GPU-free code is reachable, the
// world's and those the other modules gained. The steps that fill the headers add their tests beside this one, one
// tests/unit/<module>/test_*.cpp each.

#include <catch2/catch_test_macros.hpp>

#include <rv/world/capacity.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/nav_grid.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/platform_manifest.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/slot_navigation.hpp>
#include <rv/world/title_key.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>
#include <rv/world/world_store.hpp>

#include <rv/app/cover_requests.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/app/screen_policy.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/assets/bc1.hpp>
#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/rvtex.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/catalog/room_entry.hpp>
#include <rv/core/backups.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_array_layout.hpp>
#include <rv/scene/instance_clusters.hpp>
#include <rv/scene/pose_animator.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/ui/media_viewer_model.hpp>

TEST_CASE("world/module: the skeleton links into rv_tests with every planned header", "[world]") {
    SUCCEED("rv_world and the planned GPU-free headers of I3 are reachable");
}
