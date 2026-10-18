#pragma once

// What the scene (scene_builder.cpp) and the collection layout (collection_layout.cpp) share about a room's parts
// (I3 step 10a): a fixture's slot and level sockets, and where a dressed span's prop stands. Private to rv_world;
// defined in scene_builder.cpp.

#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include <rv/core/expected.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world::detail {

/// The socket "Slot_<level>_<index>" of `prefab`, or nullptr.
[[nodiscard]] const geometry::Socket* slotSocket(const geometry::ProceduralPrefab& prefab, std::uint32_t level,
                                                 std::uint32_t index) noexcept;
/// The socket "Level_<level>" of `prefab`, or nullptr.
[[nodiscard]] const geometry::Socket* levelSocket(const geometry::ProceduralPrefab& prefab,
                                                  std::uint32_t level) noexcept;
/// The slots of `level` in `prefab` (the indices run from 0).
[[nodiscard]] std::uint32_t slotCount(const geometry::ProceduralPrefab& prefab, std::uint32_t level) noexcept;

/// Where a dressed span's prop stands: its generator and parameters (the manifest's, with the span's free volume and
/// seed merged in) and its transform from the prop's space (pivot floor) to the room's frame.
struct DressingProp {
    std::string generator;
    nlohmann::json params = nlohmann::json::object();
    glm::mat4 transform{1.0f};
    /// The prop's nominal volume in its own space: [-w/2, 0, -d/2] .. [w/2, h, d/2] (shelf_dressing's contract).
    glm::vec3 size{0.0f};
};

/// The prop of `dressed` on `fixture` (whose prefab is `fixturePrefab`), the prop resolved as `prop`. Nothing, with the
/// reason in `reason`, when the span cannot be dressed (a model prefab, a span whose volume the prop's parameters do
/// not take). Errors: ValidationFailed for a level or slot the fixture's prefab lacks.
[[nodiscard]] Expected<std::optional<DressingProp>> dressingProp(const Room& room, const Fixture& fixture,
                                                                 const ScenePrefab& fixturePrefab,
                                                                 const DressedLevel& dressed, const ScenePrefab& prop,
                                                                 std::string& reason);

} // namespace rv::world::detail
