#pragma once

// Private to rv_world (I3 step 3): the fixtures of a fresh room in the order of ADR-0010 §4: the room at the growth
// its collection needs (the doorway's clearance is the template's: no socket reaches it), the hero corner with its
// hero-wall modules, the side-wall modules in socket order, the display fixtures, decoration on the side-wall sockets no
// module needs and on the decoration sockets within the density, and the lights within the template's limit. Where
// each prefab stands follows from its socket and its bounds: its back against the socket's back edge, centred across
// the socket's width; nothing is placed whose footprint leaves its socket.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

// The parts of the layout that the incremental update (incremental.cpp, I3 step 9) repeats on a room that stands, so
// that a module, a decoration or a wall-washer it adds is exactly what a fresh layout of that room would place. Each
// reads `input.counts.growthSteps` (the room's growth) and nothing else of the counts.

/// The section of a level by its fixture's role and its items' facing (ADR-0010 §3): featured on a display fixture,
/// band for a face-out level of a module, spine otherwise.
[[nodiscard]] Section sectionOf(FixtureRole role, LevelFacing facing) noexcept;

/// The template's side-wall module standing in `socket`, as layOutRoom() places it, with its levels. Errors: NotFound
/// for a prefab the catalog lacks, ValidationFailed for a module that does not fit the socket.
[[nodiscard]] Expected<PlacedFixture> sideModuleIn(const LayoutInput& input, const TemplateSocket& socket);

/// What decoration a socket holds, with the light of a lamp.
struct PlacedDecoration {
    PlacedFixture fixture;
    std::optional<PlanLight> light;
};
/// The decoration layOutRoom() puts in `socket`: one of the template's choices that fits it, drawn from the socket's
/// prop seed, a lamp only while `lightsLeft` is not 0, neither the prefab of a decoration of `placed` standing beside it
/// on its wall (within 1.2 m) nor a lamp within 2.2 m of another lamp there while another choice fits (I3 step 16);
/// nothing when no choice fits.
[[nodiscard]] std::optional<PlacedDecoration> decorationIn(const LayoutInput& input, const TemplateSocket& socket,
                                                           std::size_t lightsLeft, std::span<const Fixture> placed);

/// A collection module for the wall-washers: its socket and its fixture's depth.
struct ModuleOnWall {
    const TemplateSocket* socket = nullptr;
    std::int64_t depthMm = 0;
};
/// The wall-washers over `modules` (in the collection's fill order), at most `budget` of them: one per two adjacent
/// modules of a wall while the budget allows, then fewer, each over a longer run (the runs of adjacent modules cut into
/// pieces of g modules, g the smallest from 2 whose pieces the budget holds; when even one per run is too many, the
/// runs that fill first get theirs).
[[nodiscard]] std::vector<PlanLight> wallWasherLights(const LayoutInput& input, std::span<const ModuleOnWall> modules,
                                                      std::size_t budget);

} // namespace rv::world::detail
