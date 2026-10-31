#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The room's side of the inspection (ADR-0012 §4, §7; I3 step 13b), between app::WorldInteraction's InspectView
// (step 7) and the app's drawing of the held item (WorldInspection, rv_app), tested without a window:
//  - heldItemTransform(): the inspected item lifted from its pulled-out pose (pulledItemTransform(), step 13a) into the
//    hand: upright in front of the eye, its front (the cover) facing it, turned by the inspection's yaw about the
//    camera's up and its pitch about the camera's right, at the inspection's distance along the view, in the hand
//    left of and below the view's centre (heldCentre(): beside the card);
//  - heldDistanceLimit() and heldDistance(): how far the held item may stand without reaching into its own fixture
//    (a shelf level, the display case's glass);
//  - inspectDimming(): the scene behind the item dimmed by one stop and vignetted, eased with the lift;
//  - screenSelection(): the game the hero CRT follows (the quick catalog's selection while it is open, else the
//    world's focused or inspected item);
//  - I5 step 9 (ADR-0016 §11): consoleSocketOf(), the media socket of a room's console in the scene; itemExtents(),
//    insertedScale() and insertedItemTransform(), the held item going into it after Avvia; insertionDimming().

#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include <rv/app/world_interaction.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>
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

/// Where the hand holds the item in the view (I3 step 17a's review): left of and a little below the view's centre, in
/// the part of the view the card (on the right) leaves free, as tangents of the view's right and up (x right, y up):
/// at a 75-degree horizontal field of view its centre sits about a third of the way from the centre to the left edge.
inline constexpr glm::vec2 kHeldViewOffset{-0.25f, -0.08f};

/// The held item's centre at `distance` along the view from `camera`'s eye: moved sideways by kHeldViewOffset times
/// the distance up to InspectSettings' default distance (0.45 m), the offset then fading linearly to none at its
/// maxDistance (1.0 m), so that an item zoomed away returns to the view's axis, clear of its fixture's neighbours.
[[nodiscard]] glm::vec3 heldCentre(const scene::Camera& camera, float distance) noexcept;

/// The radius of the sphere that holds the item whatever its turn: half the diagonal of its slot box.
[[nodiscard]] float heldRadius(const world::CollectionItem& item) noexcept;

/// The farthest the held item's centre may stand from `eye` along `forward` (unit) so that the sphere of `radius`
/// about it stays out of `clusterBounds` grown by kHeldClearance: where the view ray enters that box, less `radius`.
/// Infinity when the ray misses the box, starts inside it, or anything is empty or not finite.
[[nodiscard]] float heldDistanceLimit(const core::Aabb& clusterBounds, float radius, const glm::vec3& eye,
                                      const glm::vec3& forward) noexcept;

/// The distance the held item is drawn at: the inspection's `distance` (the zoom, within InspectSettings' 0.3 to
/// 1.0 m), at most `limit`, never nearer than kHeldMinDistance.
[[nodiscard]] float heldDistance(float distance, float limit) noexcept;

/// The pose of the inspected item (from the item prefab's space to the room's frame, as CollectionItem::transform): at
/// `view.lift` 0 exactly `pulled` (its pulled-out pose), at 1 held in front of `camera`: its centre heldCentre() at
/// `distance` (in the hand, beside the card), its prefab axes those of the camera (+X right, +Y up, +Z its front
/// towards the eye: upright and facing it), turned by `view.yaw` about the camera's up (positive turns its front to
/// the viewer's right) and then by `view.pitch` about the camera's right (positive tilts its front up); in between, its
/// centre along the straight line and its orientation along the shorter arc (scene::interpolatePose), the lift's easing being the interaction's. The
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

/// I5 step 9: the id suffix of a hero corner's console (world::FixtureId "<room>/fx:<socket>.console").
inline constexpr std::string_view kConsoleIdSuffix = ".console";
/// The console prefabs' media socket (geometry::ConsoleCartridgeParams: "CartridgeSlot").
inline constexpr std::string_view kConsoleMediaSocket = "CartridgeSlot";
/// The hero CRT's screen (the crt prefab's "ScreenSurface": its centre, +Z out of the glass).
inline constexpr std::string_view kHeroScreenSocket = "ScreenSurface";
/// The share of the socket's opening the item fills at most once in it (its front's width and its thickness).
inline constexpr float kInsertedFill = 0.9f;
/// How far the item reaches down into the console at most once in it (meters): half of its height (a console stands
/// about 7 cm high: geometry::ConsoleCartridgeParams).
inline constexpr float kInsertedHalfDepth = 0.055f;

/// The media socket of a room's console, in the scene's frame (I5 step 9, ADR-0016 §11).
struct ConsoleSocket {
    /// From the socket's frame to the scene's: its origin the middle of the opening, +Y up out of it, +Z the console's
    /// front (the prefab's socket under the object's first instance).
    glm::mat4 transform{1.0f};
    /// The opening's width (along the socket's +X) and depth (+Z), meters.
    glm::vec2 opening{0.0f};
    /// I5 step 9's review: the screen of the room's hero CRT (kHeroScreenSocket) in the scene's frame, when the room
    /// has one: the eye turned to the console frames it too when it faces the eye (consoleViewPose()).
    std::optional<glm::mat4> heroScreen;

    [[nodiscard]] glm::vec3 position() const noexcept { return glm::vec3(transform[3]); }
    bool operator==(const ConsoleSocket&) const = default;
};

/// The media socket of the console among `description`'s objects whose ids begin with `roomPrefix` ("room:snes/"; ""
/// takes any): the first prefab object whose id ends with kConsoleIdSuffix and whose prefab (built again from its
/// kind and parameters, no GPU) has kConsoleMediaSocket, through its first instance; with the screen of the first such
/// object ending with kHeroCrtIdSuffix whose prefab has kHeroScreenSocket. Nothing in a room without a console (the
/// arcade's hero corner, a shared room's: ADR-0013 §6) or a scene without one.
[[nodiscard]] std::optional<ConsoleSocket> consoleSocketOf(const scene::SceneDescription& description,
                                                           std::string_view roomPrefix);

/// The item's size along its prefab's axes (x across its front, y up, z its thickness), meters: its slot box seen from
/// its prefab's space, scaled by its transform. Zero for an empty box.
[[nodiscard]] glm::vec3 itemExtents(const world::CollectionItem& item) noexcept;

/// The scale the item shrinks to in the socket (ADR-0016 §11: "shrinks into it"): at most 1, its front's width and its
/// thickness (`extents`' x and z) within kInsertedFill of the opening's width and depth, half its height (`extents`'
/// y) within kInsertedHalfDepth. A size or an opening that is empty or not finite sets no bound.
[[nodiscard]] float insertedScale(const glm::vec3& extents, const glm::vec2& opening) noexcept;

/// The factor of the item's scale at `progress` (clamped to [0, 1]; non-finite counts as 0) of the insertion: 1 at 0,
/// `scale` (insertedScale(); one that is not positive or not finite counts as 1) at 1, linear between.
[[nodiscard]] float insertedScaleAt(float scale, float progress) noexcept;

/// The pose of the item going into the console's media socket at `progress` (the interaction's eased insertion,
/// clamped to [0, 1]; non-finite counts as 0), from the item prefab's space to the scene's frame: at 0 exactly `held`
/// (its pose in the hand), at 1 in the socket: its centre at the socket's origin (half of it down in the console), its
/// prefab axes the socket's (upright, its front the console's front), `held`'s scale times `scale`
/// (insertedScale()); in between, its centre along the straight line, its orientation along the shorter arc
/// (scene::interpolatePose) and its scale by insertedScaleAt().
[[nodiscard]] glm::mat4 insertedItemTransform(const glm::mat4& held, const ConsoleSocket& socket, float scale,
                                              float progress) noexcept;

/// The dimming of the scene behind the item during the insertion: inspectDimming()'s at the lift, fading out with the
/// insertion (`insertion` clamped to [0, 1]), so that the console the item goes into shows as the room is lit.
[[nodiscard]] InspectDimming insertionDimming(float lift, float insertion, float baseVignette) noexcept;

} // namespace rv::app
