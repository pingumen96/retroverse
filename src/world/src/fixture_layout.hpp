#pragma once

// Private to rv_world (I3 step 3): the fixtures of a fresh room in the order of ADR-0010 §4: the room at the growth
// its collection needs (the doorway's clearance is the template's: no socket reaches it), the hero corner with its
// hero-wall modules, the side-wall modules in socket order, the display fixtures, decoration on the side-wall sockets no
// module needs and on the decoration sockets within the density, and the lights within the template's limit. Where
// each prefab stands follows from its socket and its bounds: its back against the socket's back edge, centred across
// the socket's width; nothing is placed whose footprint leaves its socket.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
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

/// Where a prefab stands in a socket: its pivot in the room's frame and its yaw.
struct SocketPose {
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
    std::int32_t yawCdeg = 0;

    bool operator==(const SocketPose&) const = default;
};

/// Where layOutRoom() stands `info`'s prefab in `socket` at `input.counts.growthSteps`: its back on the socket's back
/// edge, centred across its width (a fixture's place in its socket, its "home", I4b step 2).
[[nodiscard]] SocketPose poseInSocket(const LayoutInput& input, const TemplateSocket& socket, const PrefabInfo& info);

/// Whether a fixture of `role` moves in the editor (ADR-0015 §4): side-wall modules, display fixtures and decoration.
[[nodiscard]] bool isMovableRole(FixtureRole role) noexcept;

/// Whether `fixture` (of `input`'s room) stands elsewhere than its home, poseInSocket() at `input.counts.growthSteps`:
/// one the user moved, pinned or not (I4b step 2). A fixture of a fixed role, of a socket the template lacks or of a
/// prefab the catalog lacks never counts as moved.
[[nodiscard]] bool standsMoved(const LayoutInput& input, const Fixture& fixture);

/// The light of the lamp `lamp` (its prefab `info`, which has a light source) named `id`, as layOutRoom() places a
/// lamp's light: at the prefab's light source, turned with the fixture a quarter turn at a time.
[[nodiscard]] PlanLight lampLightOf(const Fixture& lamp, const PrefabInfo& info, FixtureId id);

/// The collection modules of a room as the wall-washers see them (wallWasherLights()): each in its template socket,
/// but a module of `moved` (those that may stand elsewhere: the editor passes every module, the update those it holds)
/// that stands elsewhere than its socket puts it (I4b step 2), in a socket made where it stands: its back edge's
/// middle, its yaw, its width and depth, on the wall its back stands against (within kMaxWallGapMm of the wall's
/// interior face, facing into the room), else on none (a run of its own). A room without moved modules is lit exactly
/// as before. `made` owns the sockets made.
struct ModulesOnWalls {
    std::vector<ModuleOnWall> modules;
    std::vector<std::unique_ptr<TemplateSocket>> made;
};
/// `modules` are the room's hero-wall and side-wall modules in the collection's fill order; one whose socket the
/// template lacks is left out.
[[nodiscard]] ModulesOnWalls modulesOnWalls(const LayoutInput& input, const Room& room,
                                            std::span<const Fixture* const> modules,
                                            const std::set<FixtureId>& moved);
/// The wall-washers over `modules` (in the collection's fill order), at most `budget` of them: one per two adjacent
/// modules of a wall while the budget allows, then fewer, each over a longer run (the runs of adjacent modules cut into
/// pieces of g modules, g the smallest from 2 whose pieces the budget holds; when even one per run is too many, the
/// runs that fill first get theirs).
[[nodiscard]] std::vector<PlanLight> wallWasherLights(const LayoutInput& input, std::span<const ModuleOnWall> modules,
                                                      std::size_t budget);

} // namespace rv::world::detail
