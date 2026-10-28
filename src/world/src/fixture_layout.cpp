#include "fixture_layout.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <rv/core/random.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/world_config.hpp>

#include "fixture_geometry.hpp"

namespace rv::world::detail {
namespace {

// Where the lights stand, from the test room's (assets/style_tests/reference_scenes/m0_testroom.json), whose look
// Gate A approved; step 16's look development may move them into the template's data. Lengths in millimetres.
constexpr std::int64_t kKeyBelowCeilingMm = 280;   ///< the key spot hangs just below the ceiling...
constexpr std::int64_t kKeyOutMm = 2300;           ///< ...this far out from the hero wall,
constexpr std::int64_t kKeyTargetOutMm = 100;      ///< aimed at the wall's foot,
constexpr std::int64_t kKeyTargetHeightMm = 500;   ///< low on the hero corner.
constexpr std::int64_t kPendantBelowCeilingMm = 1100;
constexpr std::int64_t kPictureBelowCeilingMm = 150;
constexpr std::int64_t kPictureOutMm = 700;
constexpr std::int64_t kWasherBelowCeilingMm = 250; ///< a wall-washer hangs just below the ceiling...
constexpr std::int64_t kWasherOutMm = 800;          ///< ...this far out from the modules' fronts,
constexpr std::int64_t kWasherTargetHeightMm = 1000; ///< aimed at the wall behind them at this height.
/// Two modules on one wall whose sockets are at most this far apart form one run under the wall-washers (the template's
/// side-wall sockets are 100 mm apart; a door-reserved socket breaks a run).
constexpr std::int64_t kAdjacentModuleGapMm = 200;
/// Decoration whose socket's anchor lies this close to another's on its wall stands beside it (the side-wall sockets
/// are 1 m apart; across a door-reserved socket 2.5 m).
constexpr std::int64_t kNeighbourDecorationMm = 1200;
/// A lamp standing as decoration keeps at least this far from another on its wall while another choice fits.
constexpr std::int64_t kLampSpacingMm = 2200;

/// A socket's frame: rotating +Z by an axis-aligned yaw gives the front (frontX, frontZ), rotating +X the right.
struct Axes {
    std::int64_t frontX = 0;
    std::int64_t frontZ = 1;
    std::int64_t rightX = 1;
    std::int64_t rightZ = 0;
};

Axes axesOf(std::int32_t yawCdeg) noexcept {
    switch (((yawCdeg % 36000) + 36000) % 36000) {
    case 9000:
        return {1, 0, 0, -1};
    case 18000:
        return {0, -1, -1, 0};
    case 27000:
        return {-1, 0, 0, 1};
    default:
        return {0, 1, 1, 0};
    }
}

/// The yaw rounded to the nearest quarter turn.
std::int32_t quarterTurn(std::int32_t yawCdeg) noexcept {
    const std::int32_t yaw = ((yawCdeg % 36000) + 36000) % 36000;
    return ((yaw + 4500) / 9000 % 4) * 9000;
}

struct Point {
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
};

/// A socket where it stands after the room's growth: its anchor (the middle of its back edge) and its axes.
struct Frame {
    Point anchor;
    std::int32_t yawCdeg = 0;
    Axes axes;

    /// A point of the socket's local frame (x to the right as one faces it, z out of its back edge) in the room's.
    [[nodiscard]] Point toRoom(std::int64_t localX, std::int64_t localZ) const noexcept {
        return {anchor.xMm + axes.rightX * localX + axes.frontX * localZ,
                anchor.zMm + axes.rightZ * localX + axes.frontZ * localZ};
    }
    /// How far `point` lies along the socket's right axis from its anchor.
    [[nodiscard]] std::int64_t along(const Point& point) const noexcept {
        return axes.rightX * (point.xMm - anchor.xMm) + axes.rightZ * (point.zMm - anchor.zMm);
    }
};

/// A socket of the south wall moves south with each growth step (room_template.hpp); the others stay.
Frame frameOf(const RoomTemplate& roomTemplate, const TemplateSocket& socket, std::uint32_t growthSteps) {
    const std::int64_t shift =
        socket.wall == Wall::South ? roomTemplate.growthStepMm * static_cast<std::int64_t>(growthSteps) : 0;
    return {{socket.xMm, socket.zMm + shift}, socket.yawCdeg, axesOf(socket.yawCdeg)};
}

/// The section of a level by its fixture's role and its items' facing (ADR-0010 §3).
Section levelSection(FixtureRole role, LevelFacing facing) noexcept {
    if (role == FixtureRole::Display) {
        return Section::Featured;
    }
    return facing == LevelFacing::FaceOut ? Section::Band : Section::Spine;
}

bool fitsSocket(const PrefabInfo& info, const TemplateSocket& socket) noexcept {
    return info.widthMm() <= socket.widthMm && info.depthMm() <= socket.depthMm;
}

std::uint32_t lightLimitOf(const RoomTemplate& roomTemplate) noexcept {
    return std::min(roomTemplate.lightLimit, kMaxSceneLights);
}

FixtureId fixtureIdOf(const LayoutInput& input, const std::string& socketId, std::string_view part = {}) {
    // Socket and part names are the template's, which its reader validated.
    return *FixtureId::make(input.room, socketId, part);
}

/// A point light shines from its position: its target is that position and it has no span.
PlanLight normalized(PlanLight light) noexcept {
    if (lightKind(light.role) == LightKind::Point) {
        light.targetXMm = light.xMm;
        light.targetYMm = light.yMm;
        light.targetZMm = light.zMm;
        light.spanMm = 0;
    }
    return light;
}

/// A prefab's fixture with its pivot and yaw; a collection fixture gets its prefab's levels with their sections.
PlacedFixture makeFixture(const LayoutInput& input, FixtureId id, FixtureRole role, const PrefabInfo& info,
                          const Point& pivot, std::int64_t yMm, std::int32_t yawCdeg) {
    PlacedFixture placed;
    placed.info = &info;
    Fixture& fixture = placed.fixture;
    fixture.id = std::move(id);
    fixture.role = role;
    fixture.prefab = info.ref;
    fixture.xMm = pivot.xMm;
    fixture.yMm = yMm;
    fixture.zMm = pivot.zMm;
    fixture.yawCdeg = static_cast<std::int32_t>(((yawCdeg % 36000) + 36000) % 36000);
    fixture.widthMm = info.widthMm();
    fixture.heightMm = info.heightMm();
    fixture.depthMm = info.depthMm();
    if (role == FixtureRole::HeroModule || role == FixtureRole::SideModule || role == FixtureRole::Display) {
        for (const SlotLevelInfo& level : info.levels) {
            const LevelFacing facing = level.facing(input.family, role);
            fixture.levels.push_back({level.level, level.slots, levelSection(role, facing)});
            placed.facings.push_back(facing);
        }
    }
    return placed;
}

/// A prefab standing in a socket: its back on the socket's back edge, centred across its width, lifted by `liftMm`.
PlacedFixture fixtureInSocket(const LayoutInput& input, const TemplateSocket& socket, const PrefabInfo& info,
                              FixtureRole role, std::int64_t liftMm) {
    const Frame where = frameOf(input.roomTemplate, socket, input.counts.growthSteps);
    const Point pivot = where.toRoom(-(info.minXMm + info.maxXMm) / 2, -info.minZMm);
    return makeFixture(input, fixtureIdOf(input, socket.id), role, info, pivot, liftMm, where.yawCdeg);
}

/// The lamp's light at its prefab's light source, turned with the fixture (a quarter turn at a time: the lamps' bulbs
/// stand on their axes).
PlanLight lampLight(const PlacedFixture& lamp, FixtureId id) {
    const Fixture& fixture = lamp.fixture;
    const Axes axes = axesOf(quarterTurn(fixture.yawCdeg));
    const PrefabPointMm& source = *lamp.info->lightSource;
    PlanLight light;
    light.id = std::move(id);
    light.role = LightRole::Lamp;
    light.xMm = fixture.xMm + axes.rightX * source.xMm + axes.frontX * source.zMm;
    light.yMm = fixture.yMm + source.yMm;
    light.zMm = fixture.zMm + axes.rightZ * source.xMm + axes.frontZ * source.zMm;
    return normalized(light);
}

class Builder {
public:
    explicit Builder(const LayoutInput& input) : m_input(input), m_template(input.roomTemplate) {}

    Expected<RoomLayout> build() {
        const std::uint32_t growth = m_input.counts.growthSteps;
        const Footprint interior = interiorFootprint(m_template, growth);
        Room& room = m_layout.room;
        room.id = m_input.room;
        room.templateId = m_template.id;
        room.templateVersion = m_template.version;
        room.minXMm = interior.minXMm;
        room.maxXMm = interior.maxXMm;
        room.minZMm = interior.minZMm;
        room.maxZMm = interior.maxZMm;
        room.heightMm = m_template.heightMm;
        room.growthSteps = growth;
        room.seed = m_input.roomSeed;
        room.mediaFamily = m_input.family.id;
        room.themePack = m_input.platform.themePack;

        if (Status status = preparePins(); !status) {
            return propagate(status.error());
        }
        if (Status status = placeHeroCorner(); !status) {
            return propagate(status.error());
        }
        if (Status status = placeModules(); !status) {
            return propagate(status.error());
        }
        if (Status status = placeDisplays(); !status) {
            return propagate(status.error());
        }
        placeFixedLights();
        placeWallWashers();
        placeDecoration();
        return std::move(m_layout);
    }

private:
    /// The pinned fixture of `socketId`, or null.
    const PinnedFixture* pinOf(const std::string& socketId) const {
        if (m_input.pins == nullptr) {
            return nullptr;
        }
        const auto found = m_input.pins->fixtures.find(socketId);
        return found != m_input.pins->fixtures.end() ? &found->second : nullptr;
    }

    /// The pinned fixtures standing at a transform, made first (ADR-0015 §3: before the layout fills the other
    /// sockets), so that a socket whose fixture would overlap one of them takes none.
    Status preparePins() {
        if (m_input.pins == nullptr) {
            return ok();
        }
        for (const auto& [socketId, pin] : m_input.pins->fixtures) {
            if (!pin.transform) {
                continue;
            }
            const TemplateSocket* where = socket(socketId);
            if (where == nullptr) {
                continue;
            }
            auto info = prefab(pinnedPrefab(*where, pin));
            if (!info) {
                return propagate(info.error());
            }
            m_movedPins.push_back(pinnedFixture(*where, pin, **info).fixture);
        }
        return ok();
    }

    /// The prefab a pinned fixture stands as: its own, else the template's for its socket.
    PrefabRef pinnedPrefab(const TemplateSocket& where, const PinnedFixture& pin) const {
        if (pin.prefab) {
            return *pin.prefab;
        }
        if (pin.role == FixtureRole::Display && where.prefab) {
            return *where.prefab;
        }
        return m_template.sideModule;
    }

    /// A pinned fixture as it stands: at its transform, else in its socket.
    PlacedFixture pinnedFixture(const TemplateSocket& where, const PinnedFixture& pin, const PrefabInfo& info) const {
        if (pin.transform) {
            return makeFixture(m_input, fixtureId(where.id), pin.role, info, {pin.transform->xMm, pin.transform->zMm},
                               pin.liftMm, pin.transform->yawCdeg);
        }
        return fixtureInSocket(m_input, where, info, pin.role, pin.liftMm);
    }

    /// Whether `candidate` (a fixture for the socket `socketId`) would overlap a pinned fixture standing at its
    /// transform in another socket.
    bool overlapsMovedPin(const Fixture& candidate, const std::string& socketId) const {
        return std::any_of(m_movedPins.begin(), m_movedPins.end(), [&](const Fixture& moved) {
            return moved.id.socket() != socketId && fixturesOverlap(candidate, moved);
        });
    }

    const TemplateSocket* socket(const std::string& id) const { return m_template.socket(id); }

    Frame frame(const TemplateSocket& socket) const { return frameOf(m_template, socket, m_input.counts.growthSteps); }

    Expected<const PrefabInfo*> prefab(const PrefabRef& ref) const {
        if (const PrefabInfo* info = m_input.prefabs.find(ref)) {
            return info;
        }
        return makeError(ErrorCode::NotFound, "the prefab catalog lacks " + ref.manifest + " (" + ref.kind + ")",
                         m_template.id);
    }

    FixtureId fixtureId(const std::string& socketId, std::string_view part = {}) const {
        return fixtureIdOf(m_input, socketId, part);
    }

    std::size_t addInSocket(const TemplateSocket& socket, const PrefabInfo& info, FixtureRole role,
                            std::int64_t liftMm) {
        m_layout.fixtures.push_back(fixtureInSocket(m_input, socket, info, role, liftMm));
        return m_layout.fixtures.size() - 1;
    }

    std::size_t add(FixtureId id, FixtureRole role, const PrefabInfo& info, const Point& pivot, std::int64_t yMm,
                    std::int32_t yawCdeg) {
        m_layout.fixtures.push_back(makeFixture(m_input, std::move(id), role, info, pivot, yMm, yawCdeg));
        return m_layout.fixtures.size() - 1;
    }

    Status placeHeroCorner() {
        for (const TemplateSocket& hero : m_template.sockets) {
            if (hero.kind != SocketKind::HeroCorner) {
                continue;
            }
            const Frame where = frame(hero);
            for (const HeroPart& part : hero.parts) {
                if (!part.prefab && !m_input.platform.console) {
                    continue; // the console's part, in a room without a console (ADR-0013 §2)
                }
                auto info = prefab(part.prefab ? *part.prefab : *m_input.platform.console);
                if (!info) {
                    return propagate(info.error());
                }
                const std::size_t index = add(fixtureId(hero.id, part.role), FixtureRole::HeroCorner, **info,
                                              where.toRoom(part.xMm, part.zMm), part.yMm, where.yawCdeg + part.yawCdeg);
                m_heroParts.push_back({part.role, index});
            }
            m_hero = &hero;
        }
        return ok();
    }

    Status placeModules() {
        auto hero = prefab(m_template.heroModule);
        auto side = prefab(m_template.sideModule);
        if (!hero) {
            return propagate(hero.error());
        }
        if (!side) {
            return propagate(side.error());
        }
        std::uint32_t sideModules = 0;
        for (const std::string& socketId : m_template.collectionOrder) {
            const TemplateSocket* candidate = socket(socketId);
            if (candidate == nullptr) {
                return makeError(ErrorCode::ValidationFailed, "the collection's order names no socket " + socketId,
                                 m_template.id);
            }
            if (candidate->growthStep > m_input.counts.growthSteps) {
                continue;
            }
            const bool isHero = candidate->kind == SocketKind::HeroModule;
            // A regeneration's pins (I4b step 5): a pinned decoration keeps its side-wall socket, a pinned module or
            // a pinned game's slot always takes its module, and a socket a moved pinned fixture overlaps takes none.
            const PinnedFixture* pin = isHero ? nullptr : pinOf(candidate->id);
            if (pin != nullptr && pin->role == FixtureRole::Decoration) {
                m_unneededSideSockets.push_back(candidate);
                continue;
            }
            const bool required = !isHero && m_input.pins != nullptr &&
                                  (m_input.pins->moduleSockets.contains(candidate->id) ||
                                   (pin != nullptr && pin->role == FixtureRole::SideModule));
            if (!isHero && !required && sideModules >= m_input.counts.sideModules) {
                m_unneededSideSockets.push_back(candidate);
                continue;
            }
            const PrefabInfo* info = isHero ? *hero : *side;
            if (pin != nullptr && pin->prefab) {
                auto pinned = prefab(*pin->prefab);
                if (!pinned) {
                    return propagate(pinned.error());
                }
                info = *pinned;
            }
            if (!fitsSocket(*info, *candidate)) {
                return makeError(ErrorCode::ValidationFailed,
                                 "module " + info->ref.manifest + " (" + std::to_string(info->widthMm()) + " x " +
                                     std::to_string(info->depthMm()) + " mm) does not fit socket " + candidate->id,
                                 m_template.id);
            }
            if (!isHero && !required && !m_movedPins.empty() &&
                overlapsMovedPin(fixtureInSocket(m_input, *candidate, *info, FixtureRole::SideModule, 0).fixture,
                                 candidate->id)) {
                continue; // the module goes to the next socket
            }
            if (!isHero) {
                ++sideModules;
            }
            if (pin != nullptr) {
                m_layout.fixtures.push_back(pinnedFixture(*candidate, *pin, *info));
                m_layout.modules.push_back(m_layout.fixtures.size() - 1);
                m_movedModules = m_movedModules || pin->transform.has_value();
            } else {
                m_layout.modules.push_back(
                    addInSocket(*candidate, *info, isHero ? FixtureRole::HeroModule : FixtureRole::SideModule, 0));
            }
            m_moduleSockets.push_back(candidate);
        }
        for (const std::string& socketId : m_template.heroBand) {
            for (std::size_t i = 0; i < m_layout.modules.size(); ++i) {
                if (m_moduleSockets[i]->id == socketId) {
                    m_layout.bandModules.push_back(m_layout.modules[i]);
                }
            }
        }
        return ok();
    }

    Status placeDisplays() {
        for (const std::string& socketId : m_template.displayOrder) {
            const TemplateSocket* display = socket(socketId);
            if (display == nullptr || !display->prefab) {
                return makeError(ErrorCode::ValidationFailed, "display socket " + socketId + " has no prefab",
                                 m_template.id);
            }
            const PinnedFixture* pin = pinOf(display->id);
            const bool pinned = pin != nullptr && pin->role == FixtureRole::Display;
            auto info = prefab(pinned && pin->prefab ? *pin->prefab : *display->prefab);
            if (!info) {
                return propagate(info.error());
            }
            if (!fitsSocket(**info, *display)) {
                return makeError(ErrorCode::ValidationFailed,
                                 "display " + (*info)->ref.manifest + " does not fit socket " + display->id,
                                 m_template.id);
            }
            if (pinned) {
                m_layout.fixtures.push_back(pinnedFixture(*display, *pin, **info));
                m_layout.displays.push_back(m_layout.fixtures.size() - 1);
                continue;
            }
            m_layout.displays.push_back(addInSocket(*display, **info, FixtureRole::Display, 0));
        }
        return ok();
    }

    std::uint32_t lightLimit() const noexcept { return lightLimitOf(m_template); }
    bool lightBudgetLeft() const noexcept { return m_layout.lights.size() < lightLimit(); }
    std::size_t lightsLeft() const noexcept {
        return lightLimit() - std::min<std::size_t>(lightLimit(), m_layout.lights.size());
    }

    void addLight(const PlanLight& light) {
        if (lightBudgetLeft()) {
            m_layout.lights.push_back(normalized(light));
        }
    }

    /// The key, the pendant, the hero corner's lamps and the picture light, in that order while the limit allows.
    void placeFixedLights() {
        const std::int64_t ceiling = m_template.heightMm;
        if (m_hero != nullptr) {
            // The key spot over the middle of the hero wall's collection: the hero corner and its modules.
            const Frame where = frame(*m_hero);
            std::int64_t from = -(m_hero->widthMm / 2);
            std::int64_t to = m_hero->widthMm - m_hero->widthMm / 2;
            for (const TemplateSocket* module : m_moduleSockets) {
                if (module->kind != SocketKind::HeroModule) {
                    continue;
                }
                const Frame moduleFrame = frame(*module);
                const std::int64_t a = where.along(moduleFrame.toRoom(-(module->widthMm / 2), 0));
                const std::int64_t b = where.along(moduleFrame.toRoom(module->widthMm - module->widthMm / 2, 0));
                from = std::min({from, a, b});
                to = std::max({to, a, b});
            }
            const std::int64_t middle = from + (to - from) / 2;
            const Point position = where.toRoom(middle, kKeyOutMm);
            const Point target = where.toRoom(middle, kKeyTargetOutMm);
            addLight({fixtureId(m_hero->id, "key-light"), LightRole::Key, position.xMm, ceiling - kKeyBelowCeilingMm,
                      position.zMm, target.xMm, kKeyTargetHeightMm, target.zMm, to - from});
        }
        // The pendant over the middle of the base room, on the circulation band's axis: growth never moves it.
        const Footprint base = interiorFootprint(m_template, 0);
        const std::int64_t pendantX = base.minXMm + (base.maxXMm - base.minXMm) / 2;
        const std::int64_t pendantZ = base.minZMm + (base.maxZMm - base.minZMm) / 2;
        addLight({fixtureId("room", "pendant"), LightRole::Pendant, pendantX, ceiling - kPendantBelowCeilingMm,
                  pendantZ});
        for (const auto& [role, index] : m_heroParts) {
            const PlacedFixture& part = m_layout.fixtures[index];
            if (part.info->lightSource) {
                addLight(lampLight(part, fixtureId(m_hero->id, role + "-light")));
            }
        }
        for (const auto& [role, index] : m_heroParts) {
            if (role != "sign") {
                continue;
            }
            const Fixture& sign = m_layout.fixtures[index].fixture;
            const Axes axes = axesOf(frame(*m_hero).yawCdeg);
            addLight({fixtureId(m_hero->id, "sign-light"), LightRole::Picture, sign.xMm + axes.frontX * kPictureOutMm,
                      ceiling - kPictureBelowCeilingMm, sign.zMm + axes.frontZ * kPictureOutMm, sign.xMm, sign.yMm,
                      sign.zMm, sign.widthMm});
        }
    }

    void placeWallWashers() {
        if (m_movedModules) {
            // A pinned module standing at its transform is lit where it stands, as the editor lights it (I4b step 2).
            std::vector<const Fixture*> fixtures;
            std::set<FixtureId> moved;
            for (const std::size_t index : m_layout.modules) {
                fixtures.push_back(&m_layout.fixtures[index].fixture);
                moved.insert(m_layout.fixtures[index].fixture.id);
            }
            const ModulesOnWalls onWalls = modulesOnWalls(m_input, m_layout.room, fixtures, moved);
            for (const PlanLight& light : wallWasherLights(m_input, onWalls.modules, lightsLeft())) {
                addLight(light);
            }
            return;
        }
        std::vector<ModuleOnWall> modules;
        for (std::size_t i = 0; i < m_moduleSockets.size(); ++i) {
            modules.push_back({m_moduleSockets[i], m_layout.fixtures[m_layout.modules[i]].fixture.depthMm});
        }
        for (const PlanLight& light : wallWasherLights(m_input, modules, lightsLeft())) {
            addLight(light);
        }
    }

    void decorate(const TemplateSocket& socket) {
        if (const PinnedFixture* pin = pinOf(socket.id); pin != nullptr && pin->role == FixtureRole::Decoration) {
            decoratePinned(socket, *pin);
            return;
        }
        std::vector<Fixture> placed;
        placed.reserve(m_layout.fixtures.size());
        for (const PlacedFixture& fixture : m_layout.fixtures) {
            placed.push_back(fixture.fixture);
        }
        auto decoration = decorationIn(m_input, socket, lightsLeft(), placed);
        if (!decoration) {
            return;
        }
        if (!m_movedPins.empty() && overlapsMovedPin(decoration->fixture.fixture, socket.id)) {
            return; // a pinned fixture moved there stands in its way
        }
        m_layout.fixtures.push_back(std::move(decoration->fixture));
        if (decoration->light) {
            addLight(*decoration->light);
        }
    }

    /// A pinned decoration, its prefab the pin's, at its transform or in its socket; a lamp's light while the limit
    /// allows.
    void decoratePinned(const TemplateSocket& socket, const PinnedFixture& pin) {
        const PrefabInfo* info = pin.prefab ? m_input.prefabs.find(*pin.prefab) : nullptr;
        if (info == nullptr) {
            return; // the regeneration names only measured prefabs
        }
        m_layout.fixtures.push_back(pinnedFixture(socket, pin, *info));
        if (info->lightSource && lightBudgetLeft()) {
            addLight(lampLight(m_layout.fixtures.back(), fixtureId(socket.id, "light")));
        }
    }

    bool isPinnedDecoration(const TemplateSocket& socket) const {
        const PinnedFixture* pin = pinOf(socket.id);
        return pin != nullptr && pin->role == FixtureRole::Decoration;
    }

    /// Every side-wall socket no module needs, in the collection's order, then the decoration sockets the density
    /// chooses: round(density x their count), those first whose prop seeds are lowest, in the template's order.
    void placeDecoration() {
        for (const TemplateSocket* socket : m_unneededSideSockets) {
            decorate(*socket);
        }
        struct Candidate {
            std::uint64_t seed = 0;
            const TemplateSocket* socket = nullptr;
            bool chosen = false;
        };
        std::vector<Candidate> sockets;
        for (const TemplateSocket& socket : m_template.sockets) {
            if (socket.kind == SocketKind::Decoration && socket.growthStep <= m_input.counts.growthSteps) {
                sockets.push_back({propSeed(m_input.roomSeed, fixtureId(socket.id)), &socket});
            }
        }
        const auto density =
            static_cast<std::uint64_t>(std::clamp<std::int64_t>(m_input.decorationDensityPermille, 0, kMaxPermille));
        const auto chosen = static_cast<std::size_t>((density * sockets.size() + 500) / 1000);
        std::vector<Candidate*> byDraw;
        for (Candidate& candidate : sockets) {
            byDraw.push_back(&candidate);
        }
        std::sort(byDraw.begin(), byDraw.end(), [](const Candidate* a, const Candidate* b) {
            return a->seed != b->seed ? a->seed < b->seed : a->socket->id < b->socket->id;
        });
        for (std::size_t i = 0; i < chosen; ++i) {
            byDraw[i]->chosen = true;
        }
        for (const Candidate& candidate : sockets) {
            // A pinned decoration stands whatever the density draws (I4b step 5).
            if (candidate.chosen || isPinnedDecoration(*candidate.socket)) {
                decorate(*candidate.socket);
            }
        }
    }

    const LayoutInput& m_input;
    const RoomTemplate& m_template;
    RoomLayout m_layout;
    const TemplateSocket* m_hero = nullptr;
    /// The hero corner's parts: role and fixture index.
    std::vector<std::pair<std::string, std::size_t>> m_heroParts;
    /// The socket of each of m_layout.modules.
    std::vector<const TemplateSocket*> m_moduleSockets;
    std::vector<const TemplateSocket*> m_unneededSideSockets;
    /// The pinned fixtures standing at a transform (a regeneration's), and whether a module is among them.
    std::vector<Fixture> m_movedPins;
    bool m_movedModules = false;
};

} // namespace

std::vector<PlanLight> wallWasherLights(const LayoutInput& input, std::span<const ModuleOnWall> modules,
                                        std::size_t budget) {
    const RoomTemplate& roomTemplate = input.roomTemplate;
    struct Member {
        std::size_t order = 0; ///< in the collection's fill order
        const TemplateSocket* socket = nullptr;
        std::int64_t from = 0; ///< along the wall, in the room's frame (x for north and south, z otherwise)
        std::int64_t to = 0;
    };
    std::vector<std::vector<Member>> runs;
    for (const Wall wall : {Wall::North, Wall::West, Wall::East, Wall::South, Wall::None}) {
        std::vector<Member> members;
        for (std::size_t i = 0; i < modules.size(); ++i) {
            const TemplateSocket* module = modules[i].socket;
            if (module->wall != wall) {
                continue;
            }
            const Footprint footprint = socketFootprint(roomTemplate, *module, input.counts.growthSteps);
            const bool alongX = wall == Wall::North || wall == Wall::South;
            members.push_back({i, module, alongX ? footprint.minXMm : footprint.minZMm,
                               alongX ? footprint.maxXMm : footprint.maxZMm});
        }
        std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.from < b.from; });
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i == 0 || wall == Wall::None || members[i].from - members[i - 1].to > kAdjacentModuleGapMm) {
                runs.emplace_back();
            }
            runs.back().push_back(members[i]);
        }
    }
    for (std::vector<Member>& run : runs) {
        // A run is cut from the end that fills first (the hero wall's end of a side wall).
        if (run.front().order > run.back().order) {
            std::reverse(run.begin(), run.end());
        }
    }
    std::sort(runs.begin(), runs.end(), [](const std::vector<Member>& a, const std::vector<Member>& b) {
        return a.front().order < b.front().order;
    });
    std::size_t longest = 0;
    for (const std::vector<Member>& run : runs) {
        longest = std::max(longest, run.size());
    }
    std::size_t piece = 2;
    const auto pieces = [&](std::size_t size) {
        std::size_t total = 0;
        for (const std::vector<Member>& run : runs) {
            total += (run.size() + size - 1) / size;
        }
        return total;
    };
    while (piece < longest && pieces(piece) > budget) {
        ++piece;
    }
    const std::int64_t ceiling = roomTemplate.heightMm;
    std::vector<PlanLight> lights;
    for (const std::vector<Member>& run : runs) {
        for (std::size_t first = 0; first < run.size() && lights.size() < budget; first += piece) {
            const std::size_t last = std::min(run.size(), first + piece) - 1;
            std::int64_t from = std::numeric_limits<std::int64_t>::max();
            std::int64_t to = std::numeric_limits<std::int64_t>::lowest();
            for (std::size_t i = first; i <= last; ++i) {
                from = std::min(from, run[i].from);
                to = std::max(to, run[i].to);
            }
            const TemplateSocket& socket = *run[first].socket;
            const Frame where = frameOf(roomTemplate, socket, input.counts.growthSteps);
            const std::int64_t depthMm = modules[run[first].order].depthMm;
            const bool alongX = socket.wall == Wall::North || socket.wall == Wall::South;
            const std::int64_t middle = from + (to - from) / 2;
            const Point onWall = alongX ? Point{middle, where.anchor.zMm} : Point{where.anchor.xMm, middle};
            PlanLight light;
            light.id = fixtureIdOf(input, socket.id, "washer");
            light.role = LightRole::WallWasher;
            light.xMm = onWall.xMm + where.axes.frontX * (depthMm + kWasherOutMm);
            light.yMm = ceiling - kWasherBelowCeilingMm;
            light.zMm = onWall.zMm + where.axes.frontZ * (depthMm + kWasherOutMm);
            light.targetXMm = onWall.xMm;
            light.targetYMm = kWasherTargetHeightMm;
            light.targetZMm = onWall.zMm;
            light.spanMm = to - from;
            lights.push_back(light);
        }
    }
    return lights;
}

std::optional<PlacedDecoration> decorationIn(const LayoutInput& input, const TemplateSocket& socket,
                                             std::size_t lightsLeft, std::span<const Fixture> placed) {
    // The prefabs of the decoration standing beside this socket on its wall (I3 step 16, style §13.4: no uniform
    // sequences): a choice is not repeated next to itself while another one fits.
    const Frame here = frameOf(input.roomTemplate, socket, input.counts.growthSteps);
    // Lamps keep a wider spacing: an amber pool every few metres, not a row of them.
    std::vector<PrefabRef> neighbours;
    bool lampNearby = false;
    for (const Fixture& fixture : placed) {
        if (fixture.role != FixtureRole::Decoration) {
            continue;
        }
        const TemplateSocket* other = input.roomTemplate.socket(fixture.id.socket());
        if (other == nullptr || other->wall != socket.wall || other->id == socket.id) {
            continue;
        }
        const Frame there = frameOf(input.roomTemplate, *other, input.counts.growthSteps);
        const std::int64_t dx = there.anchor.xMm - here.anchor.xMm;
        const std::int64_t dz = there.anchor.zMm - here.anchor.zMm;
        const std::int64_t squared = dx * dx + dz * dz;
        if (squared <= kNeighbourDecorationMm * kNeighbourDecorationMm) {
            neighbours.push_back(fixture.prefab);
        }
        const PrefabInfo* info = input.prefabs.find(fixture.prefab);
        if (info != nullptr && info->lightSource && squared <= kLampSpacingMm * kLampSpacingMm) {
            lampNearby = true;
        }
    }
    std::vector<std::pair<const DecorationChoice*, const PrefabInfo*>> candidates;
    std::vector<std::pair<const DecorationChoice*, const PrefabInfo*>> repeats;
    for (const DecorationChoice& choice : input.roomTemplate.decoration) {
        const PrefabInfo* info = input.prefabs.find(choice.prefab);
        if (info == nullptr || !fitsSocket(*info, socket) || (info->lightSource && lightsLeft == 0)) {
            continue;
        }
        const bool repeated = std::find(neighbours.begin(), neighbours.end(), choice.prefab) != neighbours.end() ||
                              (info->lightSource && lampNearby);
        (repeated ? repeats : candidates).emplace_back(&choice, info);
    }
    if (candidates.empty()) {
        candidates = std::move(repeats);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    core::Random random(propSeed(input.roomSeed, fixtureIdOf(input, socket.id)));
    const auto& [choice, info] = candidates[static_cast<std::size_t>(random.below(candidates.size()))];
    PlacedDecoration decoration;
    decoration.fixture = fixtureInSocket(input, socket, *info, FixtureRole::Decoration, choice->mountHeightMm);
    if (info->lightSource) {
        decoration.light = lampLight(decoration.fixture, fixtureIdOf(input, socket.id, "light"));
    }
    return decoration;
}

Expected<PlacedFixture> sideModuleIn(const LayoutInput& input, const TemplateSocket& socket) {
    const PrefabRef& ref = input.roomTemplate.sideModule;
    const PrefabInfo* info = input.prefabs.find(ref);
    if (info == nullptr) {
        return makeError(ErrorCode::NotFound, "the prefab catalog lacks " + ref.manifest + " (" + ref.kind + ")",
                         input.roomTemplate.id);
    }
    if (!fitsSocket(*info, socket)) {
        return makeError(ErrorCode::ValidationFailed,
                         "module " + info->ref.manifest + " (" + std::to_string(info->widthMm()) + " x " +
                             std::to_string(info->depthMm()) + " mm) does not fit socket " + socket.id,
                         input.roomTemplate.id);
    }
    return fixtureInSocket(input, socket, *info, FixtureRole::SideModule, 0);
}

Section sectionOf(FixtureRole role, LevelFacing facing) noexcept {
    return levelSection(role, facing);
}

SocketPose poseInSocket(const LayoutInput& input, const TemplateSocket& socket, const PrefabInfo& info) {
    const Frame where = frameOf(input.roomTemplate, socket, input.counts.growthSteps);
    const Point pivot = where.toRoom(-(info.minXMm + info.maxXMm) / 2, -info.minZMm);
    return {pivot.xMm, pivot.zMm, static_cast<std::int32_t>(((where.yawCdeg % 36000) + 36000) % 36000)};
}

bool isMovableRole(FixtureRole role) noexcept {
    return role == FixtureRole::SideModule || role == FixtureRole::Display || role == FixtureRole::Decoration;
}

bool prefabFitsSocket(const PrefabInfo& info, const TemplateSocket& socket) noexcept {
    return fitsSocket(info, socket);
}

bool standsMoved(const LayoutInput& input, const Fixture& fixture) {
    if (!isMovableRole(fixture.role)) {
        return false;
    }
    const TemplateSocket* socket = input.roomTemplate.socket(fixture.id.socket());
    const PrefabInfo* info = input.prefabs.find(fixture.prefab);
    if (socket == nullptr || info == nullptr) {
        return false;
    }
    return poseInSocket(input, *socket, *info) != SocketPose{fixture.xMm, fixture.zMm, fixture.yawCdeg};
}

PlanLight lampLightOf(const Fixture& lamp, const PrefabInfo& info, FixtureId id) {
    PlacedFixture placed;
    placed.fixture = lamp;
    placed.info = &info;
    return lampLight(placed, std::move(id));
}

ModulesOnWalls modulesOnWalls(const LayoutInput& input, const Room& room, std::span<const Fixture* const> modules,
                              const std::set<FixtureId>& moved) {
    ModulesOnWalls result;
    const RoomTemplate& roomTemplate = input.roomTemplate;
    for (const Fixture* module : modules) {
        const TemplateSocket* socket = roomTemplate.socket(module->id.socket());
        if (socket == nullptr) {
            continue;
        }
        const PrefabInfo* info = input.prefabs.find(module->prefab);
        if (info == nullptr || !moved.contains(module->id)) {
            result.modules.push_back({socket, module->depthMm});
            continue;
        }
        const SocketPose home = poseInSocket(input, *socket, *info);
        if (home == SocketPose{module->xMm, module->zMm, module->yawCdeg}) {
            result.modules.push_back({socket, module->depthMm});
            continue;
        }
        // Moved: a socket made where it stands (modules turn a quarter turn at a time), its back edge's middle the
        // anchor fixtureInSocket() would have stood it from.
        const std::int32_t yaw = quarterTurn(module->yawCdeg);
        const Axes axes = axesOf(yaw);
        const std::int64_t across = -(info->minXMm + info->maxXMm) / 2;
        const std::int64_t out = -info->minZMm;
        const Point anchor{module->xMm - axes.rightX * across - axes.frontX * out,
                           module->zMm - axes.rightZ * across - axes.frontZ * out};
        Wall wall = Wall::None;
        std::int64_t gap = -1;
        switch (yaw) {
        case 0:
            wall = Wall::North;
            gap = anchor.zMm - room.minZMm;
            break;
        case 9000:
            wall = Wall::West;
            gap = anchor.xMm - room.minXMm;
            break;
        case 18000:
            wall = Wall::South;
            gap = room.maxZMm - anchor.zMm;
            break;
        default:
            wall = Wall::East;
            gap = room.maxXMm - anchor.xMm;
            break;
        }
        if (gap < 0 || gap > kMaxWallGapMm) {
            wall = Wall::None;
        }
        auto made = std::make_unique<TemplateSocket>();
        made->id = socket->id;
        made->kind = socket->kind;
        made->wall = wall;
        made->xMm = anchor.xMm;
        // A socket of the south wall is moved by the room's growth when it is read (frameOf()): stand it back.
        made->zMm = anchor.zMm - (wall == Wall::South
                                      ? roomTemplate.growthStepMm * static_cast<std::int64_t>(input.counts.growthSteps)
                                      : 0);
        made->yawCdeg = yaw;
        made->widthMm = info->widthMm();
        made->depthMm = info->depthMm();
        result.modules.push_back({made.get(), module->depthMm});
        result.made.push_back(std::move(made));
    }
    return result;
}

Expected<RoomLayout> layOutRoom(const LayoutInput& input) {
    return Builder(input).build();
}

} // namespace rv::world::detail
