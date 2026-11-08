#pragma once

// The console rooms' history plaques (I11 step 5, ADR-0019 §6), GPU-free: where a plaque stands, the scene object it is,
// whether a ray points at it, and what it says. The plaques are no part of the plan or of the scene files: the app adds
// them to the scene shown at runtime (WorldPlaques: SceneInstance::replaceObjects() under kPlaquePrefix) for the console
// rooms whose system's identity has a description, so a world without a theme draws exactly as before.
//
// Where: on the hero corner's wall, beside the TV cabinet, in the hero corner's own free width (console_room_9x10:
// between the cabinet, x -1.225 to -0.025 m, and the featured shelf from x 0.825 m), at a reading height. It is placed
// from the room's hero sign ("<room>/fx:hero.sign", the same wall, its pivot at the back of the frame): kPlaqueOffset in
// the sign's frame.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <rv/app/identity_signs.hpp>
#include <rv/assets/history_plaque.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/ids.hpp>

namespace rv::app {

/// The plaques' object ids: "plaque:<room>".
inline constexpr std::string_view kPlaquePrefix = "plaque:";
/// The plaque's pivot (the back of its frame, at its centre) in the room's hero sign's frame: 0.955 m to the sign's
/// right and 1.15 m under it (the sign hangs at 2.5 m: the plaque's centre at 1.35 m).
inline constexpr glm::vec3 kPlaqueOffset{0.955F, -1.15F, 0.0F};
/// The plate: 42 x 30 cm in a brass frame of 1.8 cm, 2 cm deep.
inline constexpr float kPlaqueWidth = 0.42F;
inline constexpr float kPlaqueHeight = 0.30F;
inline constexpr float kPlaqueFrame = 0.018F;
inline constexpr float kPlaqueDepth = 0.02F;
/// The picture drawn for it: about 1,900 pixels a metre of plate, readable up close.
inline constexpr int kPlaquePictureWidth = 768;
inline constexpr int kPlaquePictureHeight = 548;
/// The farthest a ray picks it from (world::kMaxPickDistance's).
inline constexpr float kPlaquePickDistance = 2.5F;

/// Where a console room's plaque would stand: its room and its transform in the scene.
struct PlaqueSite {
    world::RoomId room;
    glm::mat4 transform{1.0F};

    bool operator==(const PlaqueSite&) const = default;
};

/// The plaques' sites of `description`: each console room's hero sign ("<room>/fx:hero.sign" exactly), in the objects'
/// order. Shared and arcade rooms have none.
[[nodiscard]] std::vector<PlaqueSite> plaqueSites(const scene::SceneDescription& description);

/// The plaque's id ("plaque:room:snes") and the room an id names.
[[nodiscard]] std::string plaqueId(const world::RoomId& room);
[[nodiscard]] std::optional<world::RoomId> plaqueRoom(std::string_view objectId);

/// The plaque as a scene object: a poster frame of kPlaqueWidth x kPlaqueHeight, its frame in brass, its picture a
/// paper print with a generic cover pattern (so that it has a material of its own, which WorldPlaques gives its drawn
/// plate), no shadow, no collision.
[[nodiscard]] scene::SceneObject plaqueObject(const PlaqueSite& site);

/// Where along `ray` (its origin, its unit direction) it meets the plaque at `transform`, within kPlaquePickDistance;
/// none when it misses.
[[nodiscard]] std::optional<float> plaqueHit(const glm::mat4& transform, const core::Ray& ray);

/// The plaque's box in the scene (for the hover's label above it).
[[nodiscard]] core::Aabb plaqueBounds(const glm::mat4& transform);

/// What a system's plaque says (its identity's text), none without a description: a plaque with nothing to tell is
/// not hung.
[[nodiscard]] std::optional<assets::PlaqueText> plaqueText(const SignSystem& system);

} // namespace rv::app
