#pragma once

// Private to rv_world (I3 step 3): the fixtures of a fresh room in the order of ADR-0010 §4: the room at the growth
// its collection needs (the doorway's clearance is the template's: no socket reaches it), the hero corner with its
// hero-wall modules, the side-wall modules in socket order, the display fixtures, decoration on the side-wall sockets no
// module needs and on the decoration sockets within the density, and the lights within the template's limit. Where
// each prefab stands follows from its socket and its bounds: its back against the socket's back edge, centred across
// the socket's width; nothing is placed whose footprint leaves its socket.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/platform_manifest.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world::detail {

/// What the layout reads.
struct LayoutInput {
    const RoomTemplate& roomTemplate;
    const PlatformManifest& platform;
    const MediaFamily& family;
    const PrefabCatalog& prefabs;
    RoomId room;
    std::uint64_t roomSeed = 0;
    CollectionCounts counts;
    std::int64_t decorationDensityPermille = 0;
};

/// A fixture with what was read of its prefab.
struct PlacedFixture {
    Fixture fixture;
    const PrefabInfo* info = nullptr;
    /// The facing of each of fixture.levels (in the same order).
    std::vector<LevelFacing> facings;
};

/// A fresh room's fixtures and lights.
struct RoomLayout {
    Room room;
    /// In fixture order: the hero corner's parts, the hero-wall modules, the side-wall modules, the display fixtures,
    /// the decoration.
    std::vector<PlacedFixture> fixtures;
    /// Indices into `fixtures`: the collection's modules in the collection's fill order (the hero-wall modules, then
    /// the side-wall modules), the hero band's modules in its fill order, the display fixtures in theirs.
    std::vector<std::size_t> modules;
    std::vector<std::size_t> bandModules;
    std::vector<std::size_t> displays;
    std::vector<PlanLight> lights;
};

/// Errors: ValidationFailed for a module or display prefab that does not fit its socket, a socket the orders name that
/// the template lacks; NotFound for a prefab the catalog lacks.
[[nodiscard]] Expected<RoomLayout> layOutRoom(const LayoutInput& input);

} // namespace rv::world::detail
