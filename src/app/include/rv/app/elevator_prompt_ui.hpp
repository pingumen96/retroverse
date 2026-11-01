#pragma once

// GPU-free (part of rv_app_cli, header only): the asset paths, element ids and data model that the elevator's prompt
// (rv_app's ElevatorPromptPanel, I4 step 7b; ADR-0013 §10: "a proximity zone with a prompt") shares with its RmlUi
// document (assets/ui/elevator_prompt.rml), so that GPU-free tests check the document against them
// (tests/unit/app/test_elevator_prompt_document.cpp); the name of the key or button the prompt shows; and where the
// walker must stand and look for it (facesElevator()). The words are the document's: the code passes the key's name.

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/core/aabb.hpp>

namespace rv::app {

namespace elevator_prompt {

inline constexpr std::string_view kDocumentPath = "ui/elevator_prompt.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/elevator_prompt.rcss";
/// The data model: "key" (the name of the interact key or button to press: "E", "A"), "totem" (I6 step 5: the hub's
/// directory totem's prompt, "Indice", rather than the elevator's).
inline constexpr std::string_view kDataModel = "elevator_prompt";
inline constexpr std::string_view kPanelId = "elevator-prompt";

} // namespace elevator_prompt

/// The cosine of the widest angle between the walker's view on the floor and the direction to the elevator's doors at
/// which the prompt shows (80 degrees).
inline constexpr float kFacesElevatorCosine = 0.17f;

/// The walker whose feet stand at `feet`, looking along `yawRadians` (scene::Camera's convention: yaw 0 looks towards
/// -Z, positive turns towards +X), is in front of the elevator: its feet in the proximity zone `zone` (x and z), and the
/// middle of the doors `doors` in front of it (less than 80 degrees off its view on the floor). The arrival point is in
/// the zone facing the hall, the doors behind: the prompt waits until the walker turns to them (I4 step 7b's review: a
/// prompt over the lobby's entrance read as those doors' prompt). All in one frame (the scene's).
[[nodiscard]] inline bool facesElevator(const glm::vec3& feet, float yawRadians, const core::Aabb& zone,
                                        const glm::vec3& doors) noexcept {
    if (zone.isEmpty() || feet.x < zone.min.x || feet.x > zone.max.x || feet.z < zone.min.z || feet.z > zone.max.z ||
        !std::isfinite(yawRadians)) {
        return false;
    }
    const glm::vec2 toDoors(doors.x - feet.x, doors.z - feet.z);
    const float distance = glm::length(toDoors);
    if (!(distance > 1e-4f)) {
        return true; // on the doors' middle: nothing to turn to
    }
    const glm::vec2 view(std::sin(yawRadians), -std::cos(yawRadians));
    return glm::dot(toDoors / distance, view) > kFacesElevatorCosine;
}

/// The prompt's two places (I6 step 5): a floor's elevator, the hub's directory totem.
enum class PromptPlace : std::uint8_t { Elevator, Totem };

/// The walker whose feet stand at `feet`, looking along `yawRadians`, is before the hub's directory totem (I6 step 5):
/// its feet in the totem's proximity zone and the middle of its screen in front of it (facesElevator()'s rule).
[[nodiscard]] inline bool facesTotem(const glm::vec3& feet, float yawRadians, const core::Aabb& zone,
                                     const glm::vec3& screen) noexcept {
    return facesElevator(feet, yawRadians, zone, screen);
}

} // namespace rv::app
