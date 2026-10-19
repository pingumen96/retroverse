#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The room's side of the inspection (ADR-0012 §4, §7; I3 step 13b), between app::WorldInteraction's InspectView
// (step 7) and the app's drawing of the held item (WorldInspection, rv_app), tested without a window:
//  - heldItemTransform(): the inspected item lifted from its pulled-out pose (pulledItemTransform(), step 13a) into the
//    hand: upright in front of the eye, its front (the cover) facing it, turned by the inspection's yaw about the
//    camera's up and its pitch about the camera's right, at the inspection's distance along the view;
//  - heldDistanceLimit() and heldDistance(): how far the held item may stand without reaching into its own fixture
//    (a shelf level, the display case's glass);
//  - inspectDimming(): the scene behind the item dimmed by one stop and vignetted, eased with the lift;
//  - screenSelection(): the game the hero CRT follows (the quick catalog's selection while it is open, else the
//    world's focused or inspected item).

#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include <rv/app/world_interaction.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::app {

/// The share of the light taken away from the scene behind a held item at full lift: one stop (ADR-0012 §4).
inline constexpr float kInspectDimShare = 0.5f;
/// The vignette added to the view's at full lift (PostSettings::vignette, clamped to 1).
inline constexpr float kInspectVignette = 0.25f;
/// The held item stays this far (meters) outside its cluster's box (the items of its shelf level or display case): the
/// fixture's boards, lips and glass panes stand within it.
inline constexpr float kHeldClearance = 0.06f;
/// The nearest the held item is drawn to the eye when its fixture leaves less room than the zoom asks (meters).
inline constexpr float kHeldMinDistance = 0.2f;

/// The radius of the sphere that holds the item whatever its turn: half the diagonal of its slot box.
[[nodiscard]] float heldRadius(const world::CollectionItem& item) noexcept;

/// The farthest the held item's centre may stand from `eye` along `forward` (unit) so that the sphere of `radius`
/// about it stays out of `clusterBounds` grown by kHeldClearance: where the view ray enters that box, less `radius`.
/// Infinity when the ray misses the box, starts inside it, or anything is empty or not finite.
[[nodiscard]] float heldDistanceLimit(const core::Aabb& clusterBounds, float radius, const glm::vec3& eye,
                                      const glm::vec3& forward) noexcept;

/// The distance the held item is drawn at: the inspection's `distance` (the zoom, within InspectSettings' 0.4 to
/// 1.0 m), at most `limit`, never nearer than kHeldMinDistance.
[[nodiscard]] float heldDistance(float distance, float limit) noexcept;

/// The pose of the inspected item (from the item prefab's space to the room's frame, as CollectionItem::transform): at
/// `view.lift` 0 exactly `pulled` (its pulled-out pose), at 1 held in front of `camera`: its centre `distance` along
/// the view, its prefab axes those of the camera (+X right, +Y up, +Z its front towards the eye: upright and facing
/// it), turned by `view.yaw` about the camera's up (positive turns its front to the viewer's right) and then by
/// `view.pitch` about the camera's right (positive tilts its front up); in between, its centre along the straight line
/// and its orientation along the shorter arc (scene::interpolatePose), the lift's easing being the interaction's. The
/// prefab's scale in `pulled` is kept. A non-finite lift counts as 0.
[[nodiscard]] glm::mat4 heldItemTransform(const glm::mat4& pulled, const InspectView& view,
                                          const scene::Camera& camera, float distance) noexcept;

/// The dimming of the scene behind a held item at `lift` (clamped to [0, 1]; non-finite counts as 0): the share of the
/// light taken away outside its silhouette (gl::OutlineSettings::dimOutside) and the view's vignette (`baseVignette`,
/// the scene's, raised by kInspectVignette at full lift, at most 1).
struct InspectDimming {
    float dimOutside = 0.0f;
    float vignette = 0.0f;

    bool operator==(const InspectDimming&) const = default;
};
[[nodiscard]] InspectDimming inspectDimming(float lift, float baseVignette) noexcept;

/// The game the hero CRT follows this frame (ScreenService::update's selection, ADR-0012 §7): the quick catalog's
/// selection while it is open (`catalogSelection`), else the world's item in Focus, Inspect, Launching or Returning
/// (`focusedGame`, its GameId text; empty for none); nothing in FreeRoam (the CRT keeps what it plays).
[[nodiscard]] std::optional<std::string> screenSelection(const std::optional<std::string>& catalogSelection,
                                                         WorldState state, std::string_view focusedGame);

} // namespace rv::app
