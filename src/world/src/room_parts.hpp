#pragma once

// What the scene (scene_builder.cpp) and the collection layout (collection_layout.cpp) share about a room's parts
// (I3 step 10a): a fixture's slot and level sockets, and where a dressed span's prop stands. Private to rv_world;
// defined in scene_builder.cpp.

#include <cstdint>
#include <optional>
#include <vector>
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

/// A riser card on a display case's empty level (I3 step 16): a thin poster frame ("poster_frame" parameters) as wide
/// as the span's slots less 1 cm at each end and as tall as their items, leaning on the level's panel in the slots'
/// frame; `transform` takes the card's space (pivot at its back's centre) to the room's frame.
struct RiserCard {
    nlohmann::json params = nlohmann::json::object();
    glm::mat4 transform{1.0f};
};

/// The riser card of `dressed` on `fixture`, when its prefab is a display case whose level holds the span's slots and
/// the span is at least 6 cm wide; nothing otherwise. A counter of handhelds (the display case's "counter", I6 step 6's
/// review), or any display case of a pack asking for it ("roomDressing" "cardPerSlot"), has a card in each slot of the
/// span instead, as large as the slot's item less 3 mm at each side, so that no cover is stretched across a level
/// (riserCards()).
[[nodiscard]] std::optional<RiserCard> riserCard(const Fixture& fixture, const ScenePrefab& fixturePrefab,
                                                 const DressedLevel& dressed);

/// The riser cards of `dressed` on `fixture`: riserCard()'s one card, or a card per slot (in the slots' order) on a
/// counter or with `perSlot`; empty when there is none.
[[nodiscard]] std::vector<RiserCard> riserCards(const Fixture& fixture, const ScenePrefab& fixturePrefab,
                                                const DressedLevel& dressed, bool perSlot = false);

} // namespace rv::world::detail
