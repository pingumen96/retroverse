#include <rv/world/scene_builder.hpp>

#include <rv/world/building.hpp>
#include <rv/world/gallery.hpp>
#include <rv/world/generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/validation.hpp>

#include "room_parts.hpp"

namespace rv::world {
namespace {

using nlohmann::json;

/// The walker's eye above the floor (scene::FirstPersonController's default) and where the scene's camera stands
/// inside the doorway.
constexpr double kEyeHeight = 1.65;
constexpr double kCameraInsideDoorway = 0.8;
constexpr double kCameraPitchDeg = -6.0;

/// The room shell's surfaces (the test room's: oak planks and painted plaster repeating every 2 m, a varnished
/// wainscot under a rail at 1 m, walnut trim).
constexpr double kShellMetersPerTile = 2.0;
constexpr double kShellBaseboard = 0.1;
constexpr double kShellWainscot = 1.0;

/// The shelf dressing's parameter ranges (geometry::ShelfDressingParams), in millimetres.
constexpr std::int64_t kDressingMinWidthMm = 100;
constexpr std::int64_t kDressingMaxWidthMm = 3000;
constexpr std::int64_t kDressingMinSideMm = 50;
constexpr std::int64_t kDressingMaxSideMm = 1000;

/// Invented titles for the posters and the magazines' covers (style §17.2: no real title or brand).
constexpr std::array<std::string_view, 10> kPictureTitles{
    "PIXEL PILOTS",     "SUNSET SPEEDWAY", "CASTLE OF LANTERNS", "MOONLIGHT DERBY", "NEON HARBOR",
    "STARLIGHT SAFARI", "CRYSTAL CANYON",  "TURBO TIDES",        "COMET CARNIVAL",  "LANTERN LAGOON"};

double meters(std::int64_t millimetres) noexcept {
    return static_cast<double>(millimetres) / 1000.0;
}

double degrees(std::int32_t centidegrees) noexcept {
    return static_cast<double>(centidegrees) / 100.0;
}

/// `value` rounded to 1 / `perUnit` (perUnit a power of ten), so that a float computed from the prefabs writes one text.
double rounded(double value, double perUnit) noexcept {
    const double result = std::round(value * perUnit) / perUnit;
    return result == 0.0 ? 0.0 : result; // no "-0.0"
}

json vec3Json(double x, double y, double z) {
    return json::array({x, y, z});
}

json pointJson(std::int64_t xMm, std::int64_t yMm, std::int64_t zMm) {
    return vec3Json(meters(xMm), meters(yMm), meters(zMm));
}

std::int64_t toMm(float value) noexcept {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * 1000.0));
}

/// A picture of `pattern` with an invented title and seed drawn from `seed` (the posters' covers, I6 step 3b's era
/// pictures).
json patternTexture(std::string_view pattern, std::uint64_t seed) {
    core::Random random(seed);
    const auto title = kPictureTitles[static_cast<std::size_t>(random.below(kPictureTitles.size()))];
    return json{{"pattern", std::string(pattern)}, {"title", std::string(title)}, {"seed", random.below(1000000)}};
}

/// The cabinets' marquees (I6 step 6): generic arcade words on a backlit panel, never a title (the game on a cabinet's screen
/// changes; its marquee stays) or a trade name.
constexpr std::array<std::string_view, 10> kMarqueeWords{"PLAY",      "1 UP",     "2 UP",    "HI SCORE",   "BONUS",
                                                         "PLAYER 1", "GAME ON", "TOP SCORE", "EXTRA LIFE", "LEVEL UP"};

json marqueeTexture(std::uint64_t seed) {
    core::Random random(seed);
    const auto word = kMarqueeWords[static_cast<std::size_t>(random.below(kMarqueeWords.size()))];
    return json{{"pattern", "marquee"}, {"title", std::string(word)}, {"seed", random.below(1000000)}};
}

json coverTexture(std::uint64_t seed) {
    return patternTexture("cover", seed);
}

/// The textures of a prefab kind's roles: the CRT's standby picture (I5 step 1, ADR-0016 §8: the colour bars stay the
/// test room's), a poster's or a magazine's invented cover, the
/// sign's neon lettering (I6 step 3: a neon panel in the pack's accent, its material's colour), an arcade cabinet's
/// marquees and idle screens (I6 step 6: generic words on a lit panel, in neon).
json themeTextures(std::string_view kind, bool sign, const PlatformManifest& platform, std::uint64_t seed) {
    if (kind == "crt" || kind == kSecondaryTvKind) {
        return json{{"screen", json{{"pattern", "standby"}}}};
    }
    if (kind == "poster_frame") {
        return sign ? json{{"picture", json{{"pattern", "neon"}, {"title", platform.signText}}}}
                    : json{{"picture", coverTexture(seed)}};
    }
    if (kind == "magazine_stack") {
        return json{{"cover", coverTexture(seed)}};
    }
    if (kind == "desk") {
        return json{{"screen", patternTexture("terminal", seed)}}; // I6 step 3b: the study's computer, on
    }
    if (kind == "arcade_cabinet") {
        // I6 step 6: each cabinet's marquee a lit panel with a generic word (the odd cabinets' another), and an empty
        // cabinet's screen asking for a coin, dimly.
        // Since the step's review, the row's side art (drawn where the pack names "side_art").
        return json{{"marquee", marqueeTexture(seed)},
                    {"marquee_alt", marqueeTexture(core::deriveSeed(seed, "marquee_alt"))},
                    {"screen_back", json{{"pattern", "neon"}, {"title", "INSERT COIN"}}},
                    {"side_art", json{{"pattern", "side_art"},
                                      {"seed", core::Random(core::deriveSeed(seed, "side_art")).below(1000000)}}}};
    }
    return json::object();
}

json materialsJson(const std::map<std::string, std::string>& materials) {
    json object = json::object();
    for (const auto& [role, material] : materials) {
        object[role] = material;
    }
    return object;
}

const Room* findRoom(const WorldPlan& plan, const RoomId& id) noexcept {
    for (const Room& room : plan.rooms) {
        if (room.id == id) {
            return &room;
        }
    }
    return nullptr;
}

/// A pack that lights its shelves (I6 step 3, room_look.hpp: the "board_light" role) draws them lit inside, and so its
/// showcases (I6 step 3b's review) and display cases (I6 step 6: the handheld archive's lit counters). Since I6 step
/// 6's review, a pack naming "screen" for its showcases and display cases lights their handhelds' displays, and one
/// naming "side_art" for its arcade cabinets prints their rows' outer sides.
void lightInside(json& object, std::string_view generator, const std::map<std::string, std::string>& materials) {
    if (!object["params"].is_object()) {
        return;
    }
    if ((generator == "shelf" || generator == "wall_shelf" || generator == "showcase" || generator == "display_case") &&
        materials.contains("board_light")) {
        object["params"]["boardLight"] = true;
    }
    if (generator == "showcase" && materials.contains("screen") && object["params"].value("item", "") == "handheld") {
        object["params"]["litScreen"] = true;
    }
    if (generator == "display_case" && materials.contains("screen") && object["params"].value("counter", false)) {
        object["params"]["litScreens"] = true;
    }
    if (generator == "arcade_cabinet" && materials.contains("side_art")) {
        object["params"]["sideArt"] = true;
    }
}

/// A prefab object of the scene file: the prefab as it resolved (with `params` instead of its own when given), its
/// theme's materials (those of the hero corner's part `heroPart` when the theme lists it), where it stands.
json prefabObject(std::string id, const ScenePrefab& prefab, const json* params, json position, json rotationDeg,
                  std::string_view heroPart, const SceneData& data, std::uint64_t seed) {
    const bool sign = heroPart == "sign";
    json object{{"id", std::move(id)}, {"position", std::move(position)}, {"rotationDeg", std::move(rotationDeg)}};
    if (!prefab.prefab) {
        object["model"] = prefab.model;
        return object;
    }
    object["prefab"] = prefab.generator;
    object["params"] = params != nullptr ? *params : prefab.params;
    std::map<std::string, std::string> materials = data.theme.materials(prefab.generator, heroPart);
    // A counter showcase's handhelds (I6 step 6) take the pack's showcase plastics, those of its handheld.
    if (prefab.generator == "display_case" && object["params"].value("counter", false)) {
        for (const auto& [role, material] : data.theme.materials("showcase")) {
            if (role.starts_with("plastic_")) {
                materials.emplace(role, material);
            }
        }
    }
    lightInside(object, prefab.generator, materials);
    object["materials"] = materialsJson(materials);
    if (json textures = themeTextures(prefab.generator, sign, data.platform, seed); !textures.empty()) {
        object["textures"] = std::move(textures);
    }
    // Thin geometry on walls casts no shadow (its shadow would be narrower than the filter); the rug lies on the floor
    // and is walked over.
    if (prefab.generator == "poster_frame" || prefab.generator == "rug") {
        object["castsShadows"] = false;
    }
    if (prefab.generator == "rug") {
        object["collides"] = false;
    }
    return object;
}

/// (dx, dz) millimetres of a frame turned by `yawCdeg` about +Y, in the room's frame (room_frames::yawRotation()'s
/// sense: local +X to (cos, -sin), local +Z to (sin, cos)); quarter turns exact, others rounded to the millimetre.
std::pair<std::int64_t, std::int64_t> turned(std::int64_t dx, std::int64_t dz, std::int32_t yawCdeg) noexcept {
    switch (((yawCdeg % 36000) + 36000) % 36000) {
    case 0:
        return {dx, dz};
    case 9000:
        return {dz, -dx};
    case 18000:
        return {-dx, -dz};
    case 27000:
        return {-dz, dx};
    default: {
        const double radians = static_cast<double>(yawCdeg) / 100.0 * (3.14159265358979323846 / 180.0);
        const double c = std::cos(radians);
        const double sn = std::sin(radians);
        return {std::llround(c * static_cast<double>(dx) + sn * static_cast<double>(dz)),
                std::llround(-sn * static_cast<double>(dx) + c * static_cast<double>(dz))};
    }
    }
}

/// Millimetres from meters, down and up (a hundredth of a millimetre of float noise forgiven).
std::int64_t downMm(float meters) noexcept {
    return static_cast<std::int64_t>(std::floor(std::round(static_cast<double>(meters) * 1e5) / 100.0));
}
std::int64_t upMm(float meters) noexcept {
    return static_cast<std::int64_t>(std::ceil(std::round(static_cast<double>(meters) * 1e5) / 100.0));
}

/// Grows [low, high] by `bounds` standing at (xMm, yMm, zMm) in the room, turned by `yawCdeg` about +Y.
void extendBy(glm::vec3& low, glm::vec3& high, const core::Aabb& bounds, std::int64_t xMm, std::int64_t yMm,
              std::int64_t zMm, std::int32_t yawCdeg) {
    glm::mat4 frame = room_frames::yawRotation(yawCdeg);
    frame[3] = glm::vec4(static_cast<float>(meters(xMm)), static_cast<float>(meters(yMm)),
                         static_cast<float>(meters(zMm)), 1.0f);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                              (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                              (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
        const glm::vec3 point = glm::vec3(frame * glm::vec4(local, 1.0f));
        low = glm::min(low, point);
        high = glm::max(high, point);
    }
}

/// The floor [low, high] covers, rounded outwards to millimetres.
Footprint floorOf(const glm::vec3& low, const glm::vec3& high) noexcept {
    return Footprint{downMm(low.x), downMm(low.z), upMm(high.x), upMm(high.z)};
}

/// A warning logged once per `key` in the process (a room drawn again does not repeat it).
void warnOnce(const std::string& key, const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> warned;
    const std::lock_guard lock(mutex);
    if (warned.insert(key).second) {
        spdlog::warn("{}", message);
    }
}

/// A piece of the pack's furniture where it stands in the room (I6 step 3b).
struct DrawnPiece {
    const ScenePiece* piece = nullptr;
    std::string id;
    std::int64_t xMm = 0;
    std::int64_t yMm = 0;
    std::int64_t zMm = 0;
    std::int32_t yawCdeg = 0;
    std::uint64_t seed = 0;
    /// The pack's secondary TV (I5 step 3): judged within its fixture's own prefab's footprint where it stands.
    bool secondaryTv = false;
};

/// Where a decoration's piece stands in its fixture's frame (I6 step 3b): its back on the plan's prefab's back and
/// centred across it, as the plan's prefab stood in its socket. Millimetres (dx, dz).
std::pair<std::int64_t, std::int64_t> pieceOffset(const ScenePrefab& planned, const ScenePiece& piece) noexcept {
    const core::Aabb& own = planned.prefab->bounds;
    const core::Aabb& drawn = piece.prefab.prefab->bounds;
    return {toMm(0.5f * (own.min.x + own.max.x)) - toMm(0.5f * (drawn.min.x + drawn.max.x)),
            toMm(own.min.z) - toMm(drawn.min.z)};
}

/// The pack's secondary TV fits `fixture`'s own prefab's footprint, standing as a decoration's piece stands (I5 step
/// 3): the plan's fixture keeps its collisions and navigation wherever it stands.
bool secondaryTvFits(const Fixture& fixture, const SceneData& data) {
    const ScenePrefab* own = data.prefabs.find(fixture.prefab);
    if (!data.furniture.secondaryTv || own == nullptr || !own->prefab) {
        return false;
    }
    const ScenePiece& tv = data.furniture.secondaryTv->piece;
    const auto [dx, dz] = pieceOffset(*own, tv);
    const core::Aabb& planned = own->prefab->bounds;
    const core::Aabb& drawn = tv.prefab.prefab->bounds;
    // A millimetre of rounding forgiven.
    return toMm(drawn.min.x) + dx >= toMm(planned.min.x) - 1 && toMm(drawn.max.x) + dx <= toMm(planned.max.x) + 1 &&
           toMm(drawn.min.z) + dz >= toMm(planned.min.z) - 1 && toMm(drawn.max.z) + dz <= toMm(planned.max.z) + 1;
}

/// Where the room's doorway camera stands (doorwayCamera()'s point), millimetres in the room's frame.
std::pair<std::int64_t, std::int64_t> doorwayEyeMm(const Room& room, const RoomTemplate& roomTemplate) noexcept {
    const Doorway& door = roomTemplate.doorway;
    const auto inside = static_cast<std::int64_t>(kCameraInsideDoorway * 1000.0 + 0.5);
    switch (door.wall) {
    case Wall::North:
        return {door.offsetMm, room.minZMm + inside};
    case Wall::East:
        return {room.maxXMm - inside, door.offsetMm};
    case Wall::West:
        return {room.minXMm + inside, door.offsetMm};
    case Wall::South:
    case Wall::None:
        break;
    }
    return {door.offsetMm, room.maxZMm - inside};
}

/// How far (x, z) stands from the doorway's wall into the room, and the room's extent that way (millimetres).
std::pair<std::int64_t, std::int64_t> depthFromDoorway(const Room& room, const RoomTemplate& roomTemplate,
                                                       std::int64_t x, std::int64_t z) noexcept {
    switch (roomTemplate.doorway.wall) {
    case Wall::North:
        return {z - room.minZMm, room.maxZMm - room.minZMm};
    case Wall::East:
        return {room.maxXMm - x, room.maxXMm - room.minXMm};
    case Wall::West:
        return {x - room.minXMm, room.maxXMm - room.minXMm};
    case Wall::South:
    case Wall::None:
        break;
    }
    return {room.maxZMm - z, room.maxZMm - room.minZMm};
}

/// The decoration fixtures of `room` drawn as the pack's secondary TV (scene_builder.hpp's secondaryTvFixtures()):
/// ranked by where their sockets stand at home (never where the editor moved them, so a moved sideboard keeps its TV
/// and the others theirs), in integers from the plan and the room's seed alone.
std::vector<FixtureId> chosenSecondaryTvs(const WorldPlan& plan, const Room& room, const SceneData& data) {
    std::vector<FixtureId> chosen;
    if (!data.furniture.secondaryTv || room.kind == RoomKind::Arcade) {
        return chosen;
    }
    const RoomTemplate& roomTemplate = data.roomTemplate;
    const auto middleOf = [&](const TemplateSocket& socket) {
        const Footprint home = socketFootprint(roomTemplate, socket, room.growthSteps);
        return std::pair<std::int64_t, std::int64_t>{(home.minXMm + home.maxXMm) / 2, (home.minZMm + home.maxZMm) / 2};
    };
    std::optional<std::pair<std::int64_t, std::int64_t>> hero;
    for (const TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.kind == SocketKind::HeroCorner && !hero) {
            hero = middleOf(socket);
        }
    }
    const auto [eyeX, eyeZ] = doorwayEyeMm(room, roomTemplate);
    const auto squared = [](std::int64_t dx, std::int64_t dz) { return dx * dx + dz * dz; };
    struct Candidate {
        bool preferred = false;
        std::uint64_t draw = 0;
        std::string id;
        std::int64_t toDoorway = 0;
        std::int64_t band = 0;
        const Fixture* fixture = nullptr;
    };
    std::vector<Candidate> candidates;
    for (const Fixture& fixture : plan.fixtures) {
        // A floor-standing decoration of the TV's kind whose own prefab's footprint holds it (a hung one never).
        if (fixture.id.room() != room.id || fixture.role != FixtureRole::Decoration || fixture.yMm != 0 ||
            fixture.prefab.kind != data.furniture.secondaryTv->kind || !secondaryTvFits(fixture, data)) {
            continue;
        }
        const TemplateSocket* socket = roomTemplate.socket(fixture.id.socket());
        const auto [x, z] = socket != nullptr ? middleOf(*socket) : std::pair{fixture.xMm, fixture.zMm};
        const auto [depth, extent] = depthFromDoorway(room, roomTemplate, x, z);
        Candidate candidate;
        candidate.id = fixture.id.localText();
        candidate.draw = core::deriveSeed(room.seed, "secondary_tv", candidate.id);
        candidate.toDoorway = squared(x - eyeX, z - eyeZ);
        // The room's back half (away from the doorway), or within reach of the hero corner.
        candidate.preferred = 2 * depth >= extent ||
                              (hero && squared(x - hero->first, z - hero->second) <=
                                           kSecondaryTvNearHeroMm * kSecondaryTvNearHeroMm);
        // Its third of the room's depth from the doorway (0 the doorway's, 2 the back).
        candidate.band = extent > 0 ? std::clamp<std::int64_t>(3 * depth / extent, 0, 2) : 0;
        candidate.fixture = &fixture;
        candidates.push_back(std::move(candidate));
    }
    if (candidates.empty()) {
        return chosen;
    }
    // Never the one nearest the doorway's camera (the first seen, as large as the hero): with one candidate, it is
    // that one, kept only where it is preferred.
    const auto nearest =
        std::min_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.toDoorway != b.toDoorway ? a.toDoorway < b.toDoorway : a.id < b.id;
        });
    if (candidates.size() > 1 || !nearest->preferred) {
        candidates.erase(nearest);
    }
    // The preferred first, the farthest from the doorway's camera first among them (the farther, the smaller and
    // dimmer the set the doorway sees: style 15.3's lesser screen), then the seed (sockets mirrored across the room).
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.preferred != b.preferred) {
            return a.preferred;
        }
        if (a.toDoorway != b.toDoorway) {
            return a.toDoorway > b.toDoorway;
        }
        return a.draw != b.draw ? a.draw < b.draw : a.id < b.id;
    });
    if (candidates.empty()) {
        return chosen;
    }
    chosen.push_back(candidates.front().fixture->id);
    if (room.maxZMm - room.minZMm <= kSecondaryTvTwoPastDepthMm) {
        return chosen;
    }
    // The second in another third of the room's depth (never a pair mirrored across the room), else none.
    const std::int64_t firstBand = candidates.front().band;
    const auto other = std::find_if(candidates.begin() + 1, candidates.end(),
                                    [&](const Candidate& candidate) { return candidate.band != firstBand; });
    if (other != candidates.end()) {
        chosen.push_back(other->fixture->id);
    }
    return chosen;
}

/// I12 step 4 (ADR-0021 §1): a decoration whose drawn top reaches this high is of the tall class; along a wall the
/// classes alternate and no piece stands beside its like, decorations this close along it being neighbours (the
/// plan's own rule's distance, fixture_layout.cpp).
constexpr std::int64_t kTallDecorationMm = 1500;
constexpr std::int64_t kRhythmNeighbourMm = 1200;

/// Whether `bounds` (a piece's or the plan's prefab's) stand in the tall class where `fixture` stands.
bool tallClass(const core::Aabb& bounds, const Fixture& fixture) {
    return fixture.yMm + toMm(bounds.max.y) >= kTallDecorationMm;
}

/// The variant of `variants` that `random` draws by weight from `allowed` (every one when it is empty), none when none
/// of them weighs.
const ScenePiece* drawVariant(const std::vector<std::pair<std::uint32_t, ScenePiece>>& variants, core::Random& random,
                              const std::vector<bool>& allowed) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < variants.size(); ++i) {
        total += allowed.empty() || allowed[i] ? variants[i].first : 0u;
    }
    if (total == 0) {
        return nullptr;
    }
    std::uint64_t draw = random.below(total);
    for (std::size_t i = 0; i < variants.size(); ++i) {
        const std::uint64_t weight = allowed.empty() || allowed[i] ? variants[i].first : 0u;
        if (draw < weight) {
            return &variants[i].second;
        }
        draw -= weight;
    }
    return nullptr;
}

/// The decoration variants of `room` by the rhythm of its walls (I12 step 4, ADR-0021 §1; scene_builder.hpp's file
/// comment): the decoration fixtures wall by wall in the order their sockets stand along it at home (never where the
/// editor moved them: a moved piece keeps its variant), each drawing by weight from the room's seed and its id as
/// before; beside a neighbour, a draw of the neighbour's piece or of its class (tall or low) is drawn again among the
/// variants that differ in both, else in the piece, else it stands. `secondaryTvs` stand in their sockets as low
/// pieces.
std::map<FixtureId, const ScenePiece*> chosenVariants(const WorldPlan& plan, const Room& room, const SceneData& data,
                                                      const std::vector<FixtureId>& secondaryTvs) {
    struct Spot {
        Wall wall = Wall::None;
        std::int64_t along = 0;
        std::string id;
        const Fixture* fixture = nullptr;
    };
    std::vector<Spot> spots;
    const RoomTemplate& roomTemplate = data.roomTemplate;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != room.id || fixture.role != FixtureRole::Decoration) {
            continue;
        }
        const TemplateSocket* socket = roomTemplate.socket(fixture.id.socket());
        if (socket == nullptr) {
            continue;
        }
        const Footprint home = socketFootprint(roomTemplate, *socket, room.growthSteps);
        const bool alongX = socket->wall == Wall::North || socket->wall == Wall::South;
        spots.push_back(Spot{socket->wall, alongX ? (home.minXMm + home.maxXMm) / 2 : (home.minZMm + home.maxZMm) / 2,
                             fixture.id.localText(), &fixture});
    }
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) {
        if (a.wall != b.wall) {
            return a.wall < b.wall;
        }
        return a.along != b.along ? a.along < b.along : a.id < b.id;
    });
    std::map<FixtureId, const ScenePiece*> chosen;
    const Spot* previous = nullptr;
    const ScenePiece* previousPiece = nullptr;
    // The previous piece's class: none for a hung one (a poster, a pegboard), which leaves the floor under it free and
    // takes no part in the heights' alternation.
    std::optional<bool> previousTall;
    for (const Spot& spot : spots) {
        const Fixture& fixture = *spot.fixture;
        const ScenePrefab* own = data.prefabs.find(fixture.prefab);
        const bool neighbour =
            previous != nullptr && previous->wall == spot.wall && spot.along - previous->along <= kRhythmNeighbourMm;
        const bool hung = fixture.yMm > 0;
        const ScenePiece* piece = nullptr;
        if (std::find(secondaryTvs.begin(), secondaryTvs.end(), fixture.id) != secondaryTvs.end()) {
            piece = &data.furniture.secondaryTv->piece;
        } else if (const auto found = data.furniture.decoration.find(fixture.prefab.kind);
                   found != data.furniture.decoration.end() && own != nullptr && own->prefab) {
            const auto& variants = found->second;
            core::Random random(core::deriveSeed(room.seed, "furniture", fixture.id.localText()));
            piece = drawVariant(variants, random, {});
            const auto clashes = [&](const ScenePiece* candidate, bool byClass) {
                return candidate == previousPiece ||
                       (byClass && !hung && previousTall &&
                        tallClass(candidate->prefab.prefab->bounds, fixture) == *previousTall);
            };
            if (piece != nullptr && neighbour && clashes(piece, true)) {
                for (const bool byClass : {true, false}) {
                    std::vector<bool> allowed(variants.size());
                    bool any = false;
                    for (std::size_t i = 0; i < variants.size(); ++i) {
                        allowed[i] = variants[i].first > 0 && !clashes(&variants[i].second, byClass);
                        any = any || allowed[i];
                    }
                    if (any) {
                        piece = drawVariant(variants, random, allowed);
                        break;
                    }
                }
            }
            if (piece != nullptr) {
                chosen.emplace(fixture.id, piece);
            }
        }
        previous = &spot;
        previousPiece = piece;
        if (hung) {
            previousTall.reset();
        } else {
            previousTall = piece != nullptr ? tallClass(piece->prefab.prefab->bounds, fixture)
                                            : own != nullptr && own->prefab && tallClass(own->prefab->bounds, fixture);
        }
    }
    return chosen;
}

/// What the scene builder chooses once a room (scene_builder.hpp's file comment): the sockets of the secondary TVs
/// (I5 step 3) and the decoration variants by the walls' rhythm (I12 step 4).
struct RoomChoices {
    std::vector<FixtureId> secondaryTvs;
    std::map<FixtureId, const ScenePiece*> variants;
};

RoomChoices roomChoices(const WorldPlan& plan, const Room& room, const SceneData& data) {
    RoomChoices choices;
    choices.secondaryTvs = chosenSecondaryTvs(plan, room, data);
    choices.variants = chosenVariants(plan, room, data, choices.secondaryTvs);
    return choices;
}

/// The pieces that draw `fixture` (scene_builder.hpp's file comment), before the footprint guard; nothing: the plan's
/// prefab draws it; none: it is not drawn (a hero part the pack leaves out, I6 step 6).
std::optional<std::vector<DrawnPiece>> placedPieces(const Room& room, const Fixture& fixture, const SceneData& data,
                                                    const RoomChoices& chosen) {
    const std::vector<FixtureId>& secondaryTvs = chosen.secondaryTvs;
    std::vector<DrawnPiece> pieces;
    const std::uint64_t seed = fixtureSeed(room.seed, fixture.id);
    if (fixture.role == FixtureRole::HeroCorner) {
        const auto found = data.furniture.heroParts.find(fixture.id.part());
        const TemplateSocket* socket = data.roomTemplate.socket(fixture.id.socket());
        if (found == data.furniture.heroParts.end() || socket == nullptr) {
            return std::nullopt;
        }
        const auto part = std::find_if(socket->parts.begin(), socket->parts.end(),
                                       [&](const HeroPart& candidate) { return candidate.role == fixture.id.part(); });
        if (part == socket->parts.end()) {
            return std::nullopt;
        }
        // The CRT's substitution and the parts a pack leaves out are the arcade room's (I6 step 6, ADR-0014 §6: a part
        // substitution by room kind, in a hero corner without a console); any other room drawn with the pack keeps them
        // as planned.
        if ((fixture.id.part() == kHeroTvPart || found->second.empty()) && room.kind != RoomKind::Arcade) {
            return std::nullopt;
        }
        if (found->second.empty()) {
            return pieces;
        }
        for (std::size_t i = 0; i < found->second.size(); ++i) {
            const ScenePiece& piece = found->second[i];
            const std::int64_t dx = piece.piece.xMm.value_or(part->xMm) - part->xMm;
            const std::int64_t dz = piece.piece.zMm.value_or(part->zMm) - part->zMm;
            const auto [roomX, roomZ] = turned(dx, dz, socket->yawCdeg);
            DrawnPiece drawn;
            drawn.piece = &piece;
            drawn.id = fixture.id.localText() + (i == 0 ? std::string() : "." + std::to_string(i + 1));
            drawn.xMm = fixture.xMm + roomX;
            drawn.yMm = piece.piece.yMm.value_or(fixture.yMm);
            drawn.zMm = fixture.zMm + roomZ;
            drawn.yawCdeg = ((socket->yawCdeg + piece.piece.yawCdeg.value_or(part->yawCdeg)) % 36000 + 36000) % 36000;
            drawn.seed = i == 0 ? seed : core::deriveSeed(seed, "piece", static_cast<std::uint64_t>(i));
            pieces.push_back(std::move(drawn));
        }
        return pieces;
    }
    if (fixture.role != FixtureRole::Decoration) {
        return std::nullopt;
    }
    const ScenePrefab* own = data.prefabs.find(fixture.prefab);
    // The pack's secondary TV (I5 step 3), in the sockets the room's seed picks, where its fixture stands.
    if (data.furniture.secondaryTv && fixture.prefab.kind == data.furniture.secondaryTv->kind && own != nullptr &&
        own->prefab) {
        if (std::find(secondaryTvs.begin(), secondaryTvs.end(), fixture.id) != secondaryTvs.end()) {
            const ScenePiece& tv = data.furniture.secondaryTv->piece;
            const auto [dx, dz] = pieceOffset(*own, tv);
            const auto [roomX, roomZ] = turned(dx, dz, fixture.yawCdeg);
            DrawnPiece drawn;
            drawn.piece = &tv;
            drawn.id = fixture.id.localText();
            drawn.xMm = fixture.xMm + roomX;
            drawn.yMm = fixture.yMm;
            drawn.zMm = fixture.zMm + roomZ;
            drawn.yawCdeg = fixture.yawCdeg;
            drawn.seed = seed;
            drawn.secondaryTv = true;
            pieces.push_back(std::move(drawn));
            return pieces;
        }
    }
    // The variant the room's rhythm chose (chosenVariants(): by weight from the room's seed and the fixture's id).
    const auto variant = chosen.variants.find(fixture.id);
    if (variant == chosen.variants.end() || own == nullptr || !own->prefab) {
        return std::nullopt;
    }
    const ScenePiece* piece = variant->second;
    // Its back on the socket's back edge and centred across it, where the plan's prefab stands.
    const auto [dx, dz] = pieceOffset(*own, *piece);
    const auto [roomX, roomZ] = turned(dx, dz, fixture.yawCdeg);
    DrawnPiece drawn;
    drawn.piece = piece;
    drawn.id = fixture.id.localText();
    drawn.xMm = fixture.xMm + roomX;
    drawn.yMm = fixture.yMm;
    drawn.zMm = fixture.zMm + roomZ;
    drawn.yawCdeg = fixture.yawCdeg;
    drawn.seed = seed;
    pieces.push_back(std::move(drawn));
    return pieces;
}

std::optional<std::vector<DrawnPiece>> drawnPieces(const WorldPlan& plan, const Room& room, const Fixture& fixture,
                                                   const SceneData& data, const RoomChoices& chosen);

/// Whether the hero corner of `fixture`'s socket draws its CRT as the pack's pieces (resolved, holding the screen,
/// inside the guard): only then are the parts the pack leaves out left out, the cabinet standing in their place (I6
/// step 6's review: else the CRT, drawn as planned, would float where its TV cabinet is missing).
bool heroCrtSubstituted(const WorldPlan& plan, const Room& room, const Fixture& fixture, const SceneData& data,
                        const RoomChoices& chosen) {
    for (const Fixture& candidate : plan.fixtures) {
        if (candidate.role == FixtureRole::HeroCorner && candidate.id.room() == room.id &&
            candidate.id.socket() == fixture.id.socket() && candidate.id.part() == kHeroTvPart) {
            const std::optional<std::vector<DrawnPiece>> pieces = drawnPieces(plan, room, candidate, data, chosen);
            return pieces.has_value() && !pieces->empty();
        }
    }
    return false;
}

/// The pieces that draw `fixture` (scene_builder.hpp's file comment); nothing: the plan's prefab draws it; none: it is
/// not drawn (I6 step 6: a part the pack leaves out, while its CRT stands as the pack's cabinet). The guard of ADR-0014
/// §1 (amended): a decoration's variant that leaves its socket's footprint (turned with the socket), or a hero part's
/// piece that leaves the hero corner (and the planned part's own floor, where the part reaches out of the corner: the
/// lamp beside the TV), is not drawn; the plan's prefab is, with a warning logged once. `chosen`: the room's
/// (roomChoices(), once a room).
std::optional<std::vector<DrawnPiece>> drawnPieces(const WorldPlan& plan, const Room& room, const Fixture& fixture,
                                                   const SceneData& data, const RoomChoices& chosen) {
    std::optional<std::vector<DrawnPiece>> placed = placedPieces(room, fixture, data, chosen);
    if (placed && placed->empty() && !heroCrtSubstituted(plan, room, fixture, data, chosen)) {
        warnOnce(data.theme.id + " " + data.roomTemplate.id + " " + fixture.id.localText() + " kept",
                 fmt::format("World: the pack {} leaves out {}, but its CRT is not drawn as the pack's cabinet; the "
                             "part is drawn as planned",
                             data.theme.id, fixture.id.toString()));
        return std::nullopt;
    }
    const TemplateSocket* socket = data.roomTemplate.socket(fixture.id.socket());
    if (!placed || placed->empty() || socket == nullptr) {
        return placed;
    }
    const std::vector<DrawnPiece>& pieces = *placed;
    Footprint allowed = socketFootprint(data.roomTemplate, *socket, room.growthSteps);
    // The secondary TV (I5 step 3) within its fixture's own prefab's footprint where the fixture stands: it follows a
    // sideboard the room editor moved.
    if (pieces.front().secondaryTv) {
        if (const ScenePrefab* own = data.prefabs.find(fixture.prefab); own != nullptr && own->prefab) {
            glm::vec3 low(std::numeric_limits<float>::max());
            glm::vec3 high(std::numeric_limits<float>::lowest());
            extendBy(low, high, own->prefab->bounds, fixture.xMm, fixture.yMm, fixture.zMm, fixture.yawCdeg);
            allowed = floorOf(low, high);
        }
    }
    if (fixture.role == FixtureRole::HeroCorner) {
        if (const ScenePrefab* own = data.prefabs.find(fixture.prefab); own != nullptr && own->prefab) {
            glm::vec3 low(std::numeric_limits<float>::max());
            glm::vec3 high(std::numeric_limits<float>::lowest());
            extendBy(low, high, own->prefab->bounds, fixture.xMm, fixture.yMm, fixture.zMm, fixture.yawCdeg);
            const Footprint part = floorOf(low, high);
            allowed = Footprint{std::min(allowed.minXMm, part.minXMm), std::min(allowed.minZMm, part.minZMm),
                                std::max(allowed.maxXMm, part.maxXMm), std::max(allowed.maxZMm, part.maxZMm)};
        }
    }
    // A millimetre of rounding forgiven.
    const Footprint tolerated{allowed.minXMm - 1, allowed.minZMm - 1, allowed.maxXMm + 1, allowed.maxZMm + 1};
    for (const DrawnPiece& drawn : pieces) {
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(std::numeric_limits<float>::lowest());
        extendBy(low, high, drawn.piece->prefab.prefab->bounds, drawn.xMm, drawn.yMm, drawn.zMm, drawn.yawCdeg);
        if (const Footprint floor = floorOf(low, high); !tolerated.contains(floor)) {
            warnOnce(data.theme.id + " " + data.roomTemplate.id + " " + fixture.id.localText() + " " +
                         drawn.piece->piece.prefab.manifest,
                     fmt::format("World: the pack {}'s {} leaves the footprint of {} ([{}, {}] x [{}, {}] mm, "
                                 "allowed [{}, {}] x [{}, {}]); the plan's {} is drawn instead",
                                 data.theme.id, drawn.piece->piece.prefab.manifest, fixture.id.toString(),
                                 floor.minXMm, floor.maxXMm, floor.minZMm, floor.maxZMm, allowed.minXMm,
                                 allowed.maxXMm, allowed.minZMm, allowed.maxZMm, fixture.prefab.manifest));
            return std::nullopt;
        }
        // I12 step 4: a tall variant never reaches through the ceiling (a low room's).
        if (fixture.role == FixtureRole::Decoration && toMm(high.y) > room.heightMm) {
            warnOnce(data.theme.id + " " + data.roomTemplate.id + " " + fixture.id.localText() + " " +
                         drawn.piece->piece.prefab.manifest + " ceiling",
                     fmt::format("World: the pack {}'s {} reaches {} mm, over the ceiling of {} ({} mm); the plan's {} "
                                 "is drawn instead",
                                 data.theme.id, drawn.piece->piece.prefab.manifest, toMm(high.y), room.id.toString(),
                                 room.heightMm, fixture.prefab.manifest));
            return std::nullopt;
        }
    }
    return placed;
}

/// The top of what draws `fixture` in the room's frame, millimetres (I12 step 4: a picture over it hangs over that):
/// its drawn pieces' highest point, else the plan's fixture's.
std::int64_t drawnTopMm(const WorldPlan& plan, const Room& room, const Fixture& fixture, const SceneData& data,
                        const RoomChoices& chosen) {
    const std::optional<std::vector<DrawnPiece>> pieces = drawnPieces(plan, room, fixture, data, chosen);
    if (!pieces || pieces->empty()) {
        return fixture.yMm + fixture.heightMm;
    }
    std::int64_t top = 0;
    for (const DrawnPiece& drawn : *pieces) {
        top = std::max(top, drawn.yMm + toMm(drawn.piece->prefab.prefab->bounds.max.y));
    }
    return top;
}

/// A piece's object: its prefab with its merged parameters, the pack's materials of its kind (the part's own when it
/// is of the part's kind; the secondary TV's, I5 step 3) under its own, the kind's pictures under its patterns.
json pieceObject(const DrawnPiece& drawn, const Fixture& fixture, std::string_view heroPart, const SceneData& data) {
    const ScenePiece& piece = *drawn.piece;
    const ScenePrefab& prefab = piece.prefab;
    std::map<std::string, std::string> materials = drawn.secondaryTv ? data.theme.materials(kSecondaryTvKind)
                                                   : piece.piece.prefab.kind == fixture.prefab.kind
                                                       ? data.theme.materials(prefab.generator, heroPart)
                                                       : data.theme.materials(prefab.generator);
    for (const auto& [role, material] : piece.piece.materials) {
        materials[role] = material;
    }
    json object{{"id", drawn.id},
                {"prefab", prefab.generator},
                {"params", prefab.params},
                {"position", pointJson(drawn.xMm, drawn.yMm, drawn.zMm)},
                {"rotationDeg", vec3Json(0.0, degrees(drawn.yawCdeg), 0.0)},
                {"materials", materialsJson(materials)}};
    lightInside(object, prefab.generator, materials);
    json textures = themeTextures(prefab.generator, heroPart == "sign", data.platform, drawn.seed);
    for (const auto& [role, pattern] : piece.piece.textures) {
        // A numbered role ("cover_2": a showcase's second box, I6 step 6's review) takes a picture of its own.
        const bool numbered =
            role.size() > 2 && role[role.size() - 2] == '_' && role.back() >= '0' && role.back() <= '9';
        textures[role] = patternTexture(pattern, numbered ? core::deriveSeed(drawn.seed, role) : drawn.seed);
    }
    if (!textures.empty()) {
        object["textures"] = std::move(textures);
    }
    if (prefab.generator == "poster_frame" || prefab.generator == "rug") {
        object["castsShadows"] = false;
    }
    // The hero cabinet (I6 step 6) stands right under the key light: its marquee's hood and sides would print a jagged
    // shadow across its leaning bezel (the step's review); its contact with the floor is the ambient occlusion's.
    if (prefab.generator == "arcade_cabinet" && prefab.prefab &&
        geometry::findSocket(prefab.prefab->sockets, "ScreenSurface") != nullptr) {
        object["castsShadows"] = false;
    }
    if (prefab.generator == "rug") {
        object["collides"] = false;
    }
    return object;
}

/// A piece of the hero corner's lived-in set where it stands (I12 step 3), with the part it goes with.
struct DrawnHeroDressing {
    const SceneHeroDressing* dressing = nullptr;
    const Fixture* anchor = nullptr;
    DrawnPiece drawn;
};

/// The hero corner's set of `room` (room_look.hpp's HeroDressingPiece, scene_builder.hpp's file comment), in the pack's
/// order: each piece whose anchor part the room holds and draws, at its place in the hero corner socket's frame moved
/// with the part where the plan's part stands, at its own height. A guard as the parts' keeps the data honest: a piece
/// leaving the hero corner socket's footprint, or reaching above the room's ceiling, is not drawn (a warning logged
/// once).
std::vector<DrawnHeroDressing> drawnHeroDressing(const WorldPlan& plan, const Room& room, const SceneData& data,
                                                 const RoomChoices& chosen) {
    std::vector<DrawnHeroDressing> out;
    for (const SceneHeroDressing& dressing : data.furniture.heroDressing) {
        const auto anchor = std::find_if(plan.fixtures.begin(), plan.fixtures.end(), [&](const Fixture& fixture) {
            return fixture.role == FixtureRole::HeroCorner && fixture.id.room() == room.id &&
                   fixture.id.part() == dressing.anchor;
        });
        if (anchor == plan.fixtures.end()) {
            continue;
        }
        // A part the pack leaves out (the arcade's, I6 step 6) takes its set with it.
        if (const std::optional<std::vector<DrawnPiece>> pieces = drawnPieces(plan, room, *anchor, data, chosen);
            pieces && pieces->empty()) {
            continue;
        }
        const TemplateSocket* socket = data.roomTemplate.socket(anchor->id.socket());
        if (socket == nullptr) {
            continue;
        }
        const auto part = std::find_if(socket->parts.begin(), socket->parts.end(),
                                       [&](const HeroPart& candidate) { return candidate.role == dressing.anchor; });
        if (part == socket->parts.end()) {
            continue;
        }
        const FurniturePiece& piece = dressing.piece.piece;
        const auto [roomX, roomZ] = turned(piece.xMm.value_or(part->xMm) - part->xMm,
                                           piece.zMm.value_or(part->zMm) - part->zMm, socket->yawCdeg);
        DrawnPiece drawn;
        drawn.piece = &dressing.piece;
        drawn.id = std::string(kHeroDressingObjectPrefix) + dressing.name;
        drawn.xMm = anchor->xMm + roomX;
        drawn.yMm = piece.yMm.value_or(0);
        drawn.zMm = anchor->zMm + roomZ;
        drawn.yawCdeg = ((socket->yawCdeg + piece.yawCdeg.value_or(0)) % 36000 + 36000) % 36000;
        drawn.seed = core::deriveSeed(fixtureSeed(room.seed, anchor->id), "hero-dressing", dressing.name);
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(std::numeric_limits<float>::lowest());
        extendBy(low, high, dressing.piece.prefab.prefab->bounds, drawn.xMm, drawn.yMm, drawn.zMm, drawn.yawCdeg);
        const Footprint allowed = socketFootprint(data.roomTemplate, *socket, room.growthSteps);
        // A millimetre of rounding forgiven.
        const Footprint tolerated{allowed.minXMm - 1, allowed.minZMm - 1, allowed.maxXMm + 1, allowed.maxZMm + 1};
        const Footprint floor = floorOf(low, high);
        if (!tolerated.contains(floor) || upMm(high.y) > room.heightMm) {
            warnOnce(data.theme.id + " " + data.roomTemplate.id + " hero dressing " + dressing.name,
                     fmt::format("World: the pack {}'s hero dressing {} ([{}, {}] x [{}, {}] mm, up to {}) leaves the "
                                 "hero corner ([{}, {}] x [{}, {}], up to {}); it is not drawn",
                                 data.theme.id, dressing.name, floor.minXMm, floor.maxXMm, floor.minZMm, floor.maxZMm,
                                 upMm(high.y), allowed.minXMm, allowed.maxXMm, allowed.minZMm, allowed.maxZMm,
                                 room.heightMm));
            continue;
        }
        out.push_back(DrawnHeroDressing{&dressing, &*anchor, std::move(drawn)});
    }
    return out;
}

/// The box `pieces` cover, as `fixture` is judged by the validator (drawnFixtures()): its prefab the first piece's,
/// no yaw, the pieces' bounds rounded outwards to even spans.
Fixture coveredFixture(const Fixture& fixture, const std::vector<DrawnPiece>& pieces) {
    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(std::numeric_limits<float>::lowest());
    for (const DrawnPiece& drawn : pieces) {
        extendBy(low, high, drawn.piece->prefab.prefab->bounds, drawn.xMm, drawn.yMm, drawn.zMm, drawn.yawCdeg);
    }
    std::int64_t minX = downMm(low.x);
    std::int64_t maxX = upMm(high.x);
    std::int64_t minY = downMm(low.y);
    std::int64_t maxY = upMm(high.y);
    std::int64_t minZ = downMm(low.z);
    std::int64_t maxZ = upMm(high.z);
    // Even spans, so that the centre is a whole millimetre (outwards again).
    maxX += (maxX - minX) % 2;
    maxZ += (maxZ - minZ) % 2;
    maxY += (maxY - minY) % 2;
    Fixture covered = fixture;
    covered.prefab = pieces.front().piece->prefab.ref;
    covered.yawCdeg = 0;
    covered.widthMm = maxX - minX;
    covered.depthMm = maxZ - minZ;
    covered.heightMm = maxY - minY;
    covered.xMm = (minX + maxX) / 2;
    if (covered.prefab.kind == "poster_frame") {
        // Hung from the middle of its back: the box reaches its depth out along +Z and is centred on its height.
        covered.zMm = minZ;
        covered.yMm = (minY + maxY) / 2;
    } else {
        covered.zMm = (minZ + maxZ) / 2;
        covered.yMm = minY;
    }
    return covered;
}

/// R = Ry * Rx * Rz (the scene format's order) of a rigid transform, in degrees rounded to 1e-4.
json rotationOf(const glm::mat4& m) {
    // glm is column-major: element (row r, column c) is m[c][r].
    const double r02 = m[2][0];
    const double r22 = m[2][2];
    const double r12 = m[2][1];
    const double r10 = m[0][1];
    const double r11 = m[1][1];
    constexpr double kToDegrees = 180.0 / 3.14159265358979323846;
    const double x = std::asin(std::clamp(-r12, -1.0, 1.0)) * kToDegrees;
    const double y = std::atan2(r02, r22) * kToDegrees;
    const double z = std::atan2(r10, r11) * kToDegrees;
    return vec3Json(rounded(x, 1e4), rounded(y, 1e4), rounded(z, 1e4));
}

json positionOf(const glm::mat4& m) {
    return vec3Json(rounded(m[3][0], 1e6), rounded(m[3][1], 1e6), rounded(m[3][2], 1e6));
}

json lightJson(const PlanLight& light, const LightingProfile& profile) {
    const LightStyle& style = profile.style(light.role);
    json object{{"name", light.id.localText()},
                {"position", pointJson(light.xMm, light.yMm, light.zMm)},
                {"color", style.color},
                {"intensity", style.intensity},
                {"range", style.range}};
    if (lightKind(light.role) == LightKind::Point) {
        object["type"] = "point";
    } else {
        object["type"] = "spot";
        object["target"] = pointJson(light.targetXMm, light.targetYMm, light.targetZMm);
        const double dx = meters(light.targetXMm - light.xMm);
        const double dy = meters(light.targetYMm - light.yMm);
        const double dz = meters(light.targetZMm - light.zMm);
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        constexpr double kToDegrees = 180.0 / 3.14159265358979323846;
        const double subtended = std::atan2(0.5 * meters(light.spanMm), distance) * kToDegrees;
        const double outer =
            std::clamp(std::max(subtended + style.coneMarginDeg, style.minOuterConeDeg), 1.0, kMaxOuterConeDeg);
        object["outerConeDeg"] = rounded(outer, 100.0);
        object["innerConeDeg"] = rounded(outer * std::clamp(style.innerConeRatio, 0.0, 1.0), 100.0);
    }
    if (light.role == LightRole::Key) {
        object["castsShadows"] = true;
        object["shadowStrength"] = profile.keyShadowStrength;
    }
    return object;
}

/// The camera inside the doorway at eye height, looking into the room (doorwayCamera()).
json cameraJson(const Room& room, const RoomTemplate& roomTemplate) {
    const DoorwayCamera camera = doorwayCamera(room, roomTemplate);
    return json{{"position", vec3Json(camera.x, camera.eyeY, camera.z)},
                {"yawDeg", camera.yawDeg},
                {"pitchDeg", camera.pitchDeg},
                {"horizontalFovDeg", 75.0},
                {"near", 0.05},
                {"far", 100.0}};
}

json environmentJson(const LightingProfile& profile) {
    return json{{"clearColor", profile.clearColor},
                {"ambientSky", profile.ambientSky},
                {"ambientGround", profile.ambientGround},
                {"ambientIntensity", profile.ambientIntensity},
                {"fillColor", profile.fillColor},
                {"fillIntensity", profile.fillIntensity},
                {"exposure", profile.exposure},
                {"bloomStrength", profile.bloomStrength},
                {"bloomRadius", profile.bloomRadius},
                {"vignette", profile.vignette},
                {"ssao", json{{"enabled", profile.ssaoEnabled},
                              {"radius", profile.ssaoRadius},
                              {"intensity", profile.ssaoIntensity}}}};
}

/// An accent light of the profile at its anchor's pivot and yaw (meters rounded to micrometres).
json accentJson(const AccentLight& accent, const Fixture& anchor) {
    const glm::mat4 frame = room_frames::fixtureTransform(anchor);
    const auto toRoom = [&](const glm::dvec3& local) {
        const glm::vec4 p = frame * glm::vec4(glm::vec3(local), 1.0f);
        return vec3Json(rounded(p.x, 1e6), rounded(p.y, 1e6), rounded(p.z, 1e6));
    };
    json object{{"name", "accent:" + accent.name},
                {"position", toRoom(accent.position)},
                {"color", accent.color},
                {"intensity", accent.intensity},
                {"range", accent.range}};
    if (accent.spot) {
        object["type"] = "spot";
        object["target"] = toRoom(accent.target);
        object["outerConeDeg"] = accent.outerConeDeg;
        object["innerConeDeg"] = accent.innerConeDeg;
    } else {
        object["type"] = "point";
    }
    return object;
}

/// A riser card (detail::riserCard) as a poster frame of the theme's "riser_card" materials with an invented cover.
json riserCardObject(std::string id, const detail::RiserCard& card, const SceneData& data, std::uint64_t seed) {
    json object{{"id", std::move(id)},
                {"prefab", "poster_frame"},
                {"params", card.params},
                {"position", positionOf(card.transform)},
                {"rotationDeg", rotationOf(card.transform)},
                {"materials", materialsJson(data.theme.materials("riser_card"))},
                {"textures", json{{"picture", coverTexture(seed)}}},
                {"castsShadows", false},
                {"collides", false}};
    return object;
}

/// How far a fixture's back may stand from a wall to count as standing against it (the pack's wall pictures).
constexpr std::int64_t kAgainstWallMm = 200;
/// The pack's wall pictures stand off their wall by this much, their frame this wide and deep.
constexpr std::int64_t kWallPictureOffMm = 10;
constexpr std::int64_t kWallPictureFrameMm = 25;
constexpr std::int64_t kWallPictureDepthMm = 30;
/// The cove strip: its depth from the wall and its height.
constexpr std::int64_t kCoveDepthMm = 45;
constexpr std::int64_t kCoveHeightMm = 30;
/// The cove stops this far short of the room's corners; its washes stand this far off the wall, the lower one this
/// tall.
constexpr std::int64_t kCoveCornerMm = 150;
constexpr std::int64_t kCoveWashOffMm = 4;
constexpr std::int64_t kCoveWashBelowMm = 500;
/// A spot's can: its radius and height, the gap under it to the light, its lens and its stem.
constexpr std::int64_t kCanRadiusMm = 55;
constexpr std::int64_t kCanHeightMm = 120;
constexpr std::int64_t kCanGapMm = 15;
constexpr std::int64_t kLensRadiusMm = 42;
constexpr std::int64_t kLensHeightMm = 4;
constexpr std::int64_t kStemRadiusMm = 8;
/// The runner's thickness (a rug's is 12 mm).
constexpr std::int64_t kRunnerThicknessMm = 8;

/// I12 step 1 (ADR-0021 §1): the room's architecture beyond its room_shell. The picture rail's section; the
/// wallpaper's and the frieze's skin over the plaster; the cornice's section under the ceiling; the beams' across it.
constexpr std::int64_t kPictureRailHeightMm = 45;
constexpr std::int64_t kPictureRailDepthMm = 28;
constexpr std::int64_t kWallSkinDepthMm = 5;
constexpr std::int64_t kCorniceHeightMm = 90;
constexpr std::int64_t kCorniceDepthMm = 60;
constexpr std::int64_t kBeamWidthMm = 180;
constexpr std::int64_t kBeamDepthMm = 200;
/// The beams stop this far short of the room's south wall.
constexpr std::int64_t kBeamEndClearMm = 1000;
/// The room_shell's casing around its doorway (geometry::RoomShellParams) and its wainscot's top (kShellWainscot).
constexpr std::int64_t kDoorCasingMm = 70;
constexpr std::int64_t kWainscotTopMm = 1000;
static_assert(kShellWainscot * 1000.0 == static_cast<double>(kWainscotTopMm));
/// Meters per uv unit: the damask's repeat, the troweled plaster's, wood's.
constexpr double kWallpaperMetersPerTile = 1.0;
constexpr double kPlasterMetersPerTile = 2.0;
constexpr double kWoodMetersPerTile = 1.0;
/// The pendants: a drum this wide and tall, closed by a diffuser below and a metal top, on a stem under a canopy.
constexpr std::int64_t kPendantRadiusMm = 210;
constexpr std::int64_t kPendantHeightMm = 220;
constexpr std::int64_t kPendantDiscMm = 4;
constexpr std::int64_t kPendantStemRadiusMm = 6;
constexpr std::int64_t kPendantCanopyRadiusMm = 55;
constexpr std::int64_t kPendantCanopyHeightMm = 25;
constexpr int kPendantSegments = 32;
/// The look pendant near the doorway: its light hangs this far under the ceiling (the plan's pendant's height), this
/// far into the room from the doorway's middle, the drum's bottom this far under it; its share of the profile's
/// pendant.
constexpr std::int64_t kLookPendantBelowCeilingMm = 1100;
constexpr std::int64_t kLookPendantFromDoorwayMm = 1500;
constexpr std::int64_t kLookPendantDrumBelowLightMm = 20;
constexpr double kLookPendantShare = 0.5;
/// Its light a spot straight down (I12 step 4): a pool about 1.9 m across on the floor under it, the doorway's wall
/// beside the opening out of its cone.
constexpr double kLookPendantOuterConeDeg = 42.0;
constexpr double kLookPendantInnerConeDeg = 20.0;
/// The spot cans over the display cases (the spots' cans above) hang their lens this high: small warm points at the top
/// of the doorway's view, as the concept's ceiling lights, where drums there filled it (I12 step 1's review).
constexpr std::int64_t kDisplayCanBottomMm = 2350;

/// A primitive of the pack's room dressing: no collision, no shadow.
json dressingPrimitive(std::string id, std::string_view primitive, json params, json position, double yawDeg,
                       const std::string& material) {
    return json{{"id", std::move(id)},
                {"primitive", std::string(primitive)},
                {"params", std::move(params)},
                {"position", std::move(position)},
                {"rotationDeg", vec3Json(0.0, yawDeg, 0.0)},
                {"material", material},
                {"castsShadows", false},
                {"collides", false}};
}

/// One wall's run along it (millimetres: x for the north and south walls, z for the west and east ones).
struct WallRun {
    Wall wall = Wall::North;
    std::int64_t from = 0;
    std::int64_t to = 0;
};

/// The room's four walls: the north and south ones corner to corner, the west and east ones between them inset by
/// `inset` (a band's depth), so that the bands of one height meet at the corners without overlapping.
std::array<WallRun, 4> wallRuns(const Room& room, std::int64_t inset) {
    return {WallRun{Wall::North, room.minXMm, room.maxXMm}, WallRun{Wall::South, room.minXMm, room.maxXMm},
            WallRun{Wall::West, room.minZMm + inset, room.maxZMm - inset},
            WallRun{Wall::East, room.minZMm + inset, room.maxZMm - inset}};
}

/// A box on `wall`'s inner face from `from` to `to` along it, `depth` proud of it, from `bottom` to `top` above the
/// floor, of `material`: its uvs along the grain (wood), or in meters anchored at its place (paper, plaster: the pieces
/// of a wall, and the walls at one height, tile on without a seam).
json wallBand(std::string id, const Room& room, Wall wall, std::int64_t from, std::int64_t to, std::int64_t depth,
              std::int64_t bottom, std::int64_t top, const std::string& material, bool grain, double metersPerTile) {
    const double along = static_cast<double>(from + to) / 2000.0;
    const double y = static_cast<double>(bottom + top) / 2000.0;
    const double length = meters(to - from);
    const double height = meters(top - bottom);
    json position;
    json size;
    switch (wall) {
    case Wall::North:
        position = vec3Json(along, y, static_cast<double>(2 * room.minZMm + depth) / 2000.0);
        size = vec3Json(length, height, meters(depth));
        break;
    case Wall::South:
        position = vec3Json(along, y, static_cast<double>(2 * room.maxZMm - depth) / 2000.0);
        size = vec3Json(length, height, meters(depth));
        break;
    case Wall::West:
        position = vec3Json(static_cast<double>(2 * room.minXMm + depth) / 2000.0, y, along);
        size = vec3Json(meters(depth), height, length);
        break;
    case Wall::East:
    case Wall::None:
        position = vec3Json(static_cast<double>(2 * room.maxXMm - depth) / 2000.0, y, along);
        size = vec3Json(meters(depth), height, length);
        break;
    }
    json params{{"size", std::move(size)}, {"uv", grain ? "grain" : "meters"}, {"metersPerTile", metersPerTile}};
    if (!grain) {
        params["uvOrigin"] = position;
    }
    return dressingPrimitive(std::move(id), "box", std::move(params), std::move(position), 0.0, material);
}

/// The band from `bottom` to `top` along every wall (wallRuns(), inset by its depth), split around the doorway's
/// opening and casing where it reaches them, the piece over the casing up to its top.
void addWallBand(json& objects, std::string_view id, const Room& room, const Doorway& door, std::int64_t depth,
                 std::int64_t bottom, std::int64_t top, const std::string& material, bool grain, double metersPerTile) {
    if (top - bottom < 5) {
        return;
    }
    const std::int64_t doorTop = door.heightMm + kDoorCasingMm;
    for (const WallRun& run : wallRuns(room, depth)) {
        const std::string wallId = std::string(id) + "-" + std::string(toString(run.wall));
        if (door.wall != run.wall || bottom >= doorTop) {
            objects.push_back(
                wallBand(wallId, room, run.wall, run.from, run.to, depth, bottom, top, material, grain, metersPerTile));
            continue;
        }
        const std::int64_t gapFrom = door.offsetMm - door.widthMm / 2 - kDoorCasingMm;
        const std::int64_t gapTo = door.offsetMm + door.widthMm / 2 + kDoorCasingMm;
        if (gapFrom - run.from >= 10) {
            objects.push_back(wallBand(wallId + "/a", room, run.wall, run.from, gapFrom, depth, bottom, top, material,
                                       grain, metersPerTile));
        }
        if (run.to - gapTo >= 10) {
            objects.push_back(wallBand(wallId + "/b", room, run.wall, gapTo, run.to, depth, bottom, top, material,
                                       grain, metersPerTile));
        }
        if (top - std::max(bottom, doorTop) >= 5) {
            objects.push_back(wallBand(wallId + "/over", room, run.wall, gapFrom, gapTo, depth,
                                       std::max(bottom, doorTop), top, material, grain, metersPerTile));
        }
    }
}

/// The z of the room's beams (ShellDressing): every beamPitchMm from its north wall while kBeamEndClearMm short of its
/// south wall; none without beams.
std::vector<std::int64_t> beamPlaces(const Room& room, const ShellDressing& shell) {
    std::vector<std::int64_t> places;
    if (shell.beams.empty() || shell.beamPitchMm <= 0) {
        return places;
    }
    for (std::int64_t z = room.minZMm + shell.beamPitchMm; z <= room.maxZMm - kBeamEndClearMm; z += shell.beamPitchMm) {
        places.push_back(z);
    }
    return places;
}

/// The room's architecture beyond its room_shell (I12 step 1, ADR-0021 §1): the wallpaper from the wainscot's rail up
/// to the picture rail, the rail, the frieze above it, the cornice under the ceiling, the beams across it.
void addShellDressing(json& objects, const Room& room, const Doorway& door, const ShellDressing& shell) {
    const std::int64_t height = room.heightMm;
    const std::int64_t corniceBottom = shell.cornice.empty() ? height : height - kCorniceHeightMm;
    const bool rail = !shell.rail.empty() && shell.railYMm - kPictureRailHeightMm > kWainscotTopMm &&
                      shell.railYMm + 50 < corniceBottom;
    const std::int64_t railBottom = shell.railYMm - kPictureRailHeightMm;
    if (!shell.wallpaper.empty()) {
        addWallBand(objects, "dressing:shell/wallpaper", room, door, kWallSkinDepthMm, kWainscotTopMm,
                    rail ? railBottom : corniceBottom, shell.wallpaper, false, kWallpaperMetersPerTile);
    }
    if (rail) {
        addWallBand(objects, "dressing:shell/rail", room, door, kPictureRailDepthMm, railBottom, shell.railYMm,
                    shell.rail, true, kWoodMetersPerTile);
        if (!shell.frieze.empty()) {
            addWallBand(objects, "dressing:shell/frieze", room, door, kWallSkinDepthMm, shell.railYMm, corniceBottom,
                        shell.frieze, false, kPlasterMetersPerTile);
        }
    }
    if (!shell.cornice.empty()) {
        addWallBand(objects, "dressing:shell/cornice", room, door, kCorniceDepthMm, corniceBottom, height,
                    shell.cornice, true, kWoodMetersPerTile);
    }
    for (const std::int64_t z : beamPlaces(room, shell)) {
        objects.push_back(dressingPrimitive(
            "dressing:shell/beam:" + std::to_string(z - room.minZMm), "box",
            json{{"size", vec3Json(meters(room.maxXMm - room.minXMm), meters(kBeamDepthMm), meters(kBeamWidthMm))},
                 {"uv", "grain"},
                 {"metersPerTile", kWoodMetersPerTile}},
            vec3Json(static_cast<double>(room.minXMm + room.maxXMm) / 2000.0,
                     static_cast<double>(2 * height - kBeamDepthMm) / 2000.0, meters(z)),
            0.0, shell.beams));
    }
}

/// Where the look pendant hangs (I12 step 1): kLookPendantFromDoorwayMm into the room from the doorway's middle; none
/// for a room without a doorway.
std::optional<std::pair<std::int64_t, std::int64_t>> lookPendantPlace(const Room& room, const Doorway& door) {
    switch (door.wall) {
    case Wall::North:
        return std::pair{door.offsetMm, room.minZMm + kLookPendantFromDoorwayMm};
    case Wall::South:
        return std::pair{door.offsetMm, room.maxZMm - kLookPendantFromDoorwayMm};
    case Wall::West:
        return std::pair{room.minXMm + kLookPendantFromDoorwayMm, door.offsetMm};
    case Wall::East:
        return std::pair{room.maxXMm - kLookPendantFromDoorwayMm, door.offsetMm};
    case Wall::None:
        break;
    }
    return std::nullopt;
}

/// What a lamp hanging at `z` hangs from: the ceiling or, where a beam passes over it within `reach`, the beam's
/// underside.
std::int64_t ceilingOver(const Room& room, const std::vector<std::int64_t>& beams, std::int64_t z, std::int64_t reach) {
    for (const std::int64_t beam : beams) {
        if (std::abs(z - beam) <= kBeamWidthMm / 2 + reach) {
            return room.heightMm - kBeamDepthMm;
        }
    }
    return room.heightMm;
}

/// A drum lamp hanging at (x, z), its drum's bottom `bottom` above the floor, from the ceiling or, under a beam, from
/// the beam's underside: a canopy, a stem, the drum's shade (open, lit from inside), its metal top and the diffuser
/// closing it below. Left out where the ceiling (or the beam) leaves it no room.
void addPendant(json& objects, const std::string& id, const Room& room, const std::vector<std::int64_t>& beams,
                const PendantDressing& lamps, std::int64_t x, std::int64_t z, std::int64_t bottom) {
    const std::int64_t ceiling = ceilingOver(room, beams, z, kPendantCanopyRadiusMm);
    const std::int64_t top = bottom + kPendantHeightMm;
    const std::int64_t stemTop = ceiling - kPendantCanopyHeightMm;
    if (stemTop - top < 20) {
        return;
    }
    const auto disc = [&](std::string suffix, std::int64_t radius, std::int64_t centreY2, const std::string& material,
                          std::int64_t heightMm, bool capped) {
        objects.push_back(dressingPrimitive(id + suffix, "cylinder",
                                            json{{"radius", meters(radius)},
                                                 {"height", meters(heightMm)},
                                                 {"segments", kPendantSegments},
                                                 {"capped", capped}},
                                            vec3Json(meters(x), static_cast<double>(centreY2) / 2000.0, meters(z)), 0.0,
                                            material));
    };
    disc("/drum", kPendantRadiusMm, bottom + top, lamps.shade, kPendantHeightMm, false);
    disc("/top", kPendantRadiusMm, 2 * top - kPendantDiscMm, lamps.metal, kPendantDiscMm, true);
    disc("/diffuser", kPendantRadiusMm - 6, 2 * bottom + 3 * kPendantDiscMm, lamps.diffuser, kPendantDiscMm, true);
    disc("/stem", kPendantStemRadiusMm, top + stemTop, lamps.metal, stemTop - top, true);
    disc("/canopy", kPendantCanopyRadiusMm, stemTop + ceiling, lamps.metal, kPendantCanopyHeightMm, true);
}

/// A spot can hanging at (x, z), its lens's bottom `bottom` above the floor: the arcade's spots' can of the pack's
/// metal, its lens the pendants' diffuser, on a stem from the ceiling or a beam's underside. Left out where they
/// leave it no room.
void addPendantCan(json& objects, const std::string& id, const Room& room, const std::vector<std::int64_t>& beams,
                   const PendantDressing& lamps, std::int64_t x, std::int64_t z, std::int64_t bottom) {
    const std::int64_t ceiling = ceilingOver(room, beams, z, kPendantStemRadiusMm);
    const std::int64_t canBottom = bottom + kLensHeightMm;
    const std::int64_t top = canBottom + kCanHeightMm;
    if (ceiling - top < 20) {
        return;
    }
    const auto part = [&](std::string suffix, std::int64_t radius, std::int64_t from, std::int64_t to,
                          const std::string& material, int segments) {
        objects.push_back(
            dressingPrimitive(id + suffix, "cylinder",
                              json{{"radius", meters(radius)}, {"height", meters(to - from)}, {"segments", segments}},
                              vec3Json(meters(x), static_cast<double>(from + to) / 2000.0, meters(z)), 0.0, material));
    };
    part("/can", kCanRadiusMm, canBottom, top, lamps.metal, 20);
    part("/lens", kLensRadiusMm, bottom, canBottom, lamps.diffuser, 20);
    part("/stem", kPendantStemRadiusMm, top, ceiling, lamps.metal, 8);
}

/// The pack's pendants (I12 step 1, ADR-0021 §1): the look pendant's drum near the doorway (its light:
/// lookPendantLight()) and a spot can over each display fixture standing free of the walls (its accent light, the
/// profile's case light, is the can's).
void addPendants(json& objects, const WorldPlan& plan, const Room& room, const SceneData& data,
                 const PendantDressing& lamps) {
    const RoomDressing& dressing = data.theme.dressing;
    const std::vector<std::int64_t> beams =
        dressing.shell ? beamPlaces(room, *dressing.shell) : std::vector<std::int64_t>{};
    if (const auto place = lookPendantPlace(room, data.roomTemplate.doorway)) {
        addPendant(objects, "dressing:pendant:doorway", room, beams, lamps, place->first, place->second,
                   room.heightMm - kLookPendantBelowCeilingMm - kLookPendantDrumBelowLightMm);
    }
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != room.id || fixture.role != FixtureRole::Display) {
            continue;
        }
        // Standing free: its back on no wall (a wall shelf hangs on one).
        const auto [backX, backZ] = turned(0, -fixture.depthMm / 2, fixture.yawCdeg);
        const std::int64_t x = fixture.xMm + backX;
        const std::int64_t z = fixture.zMm + backZ;
        if (x - room.minXMm <= kAgainstWallMm || room.maxXMm - x <= kAgainstWallMm ||
            z - room.minZMm <= kAgainstWallMm || room.maxZMm - z <= kAgainstWallMm) {
            continue;
        }
        addPendantCan(objects, "dressing:pendant:" + fixture.id.localText(), room, beams, lamps, fixture.xMm,
                      fixture.zMm, kDisplayCanBottomMm);
    }
}

/// The look pendant's light (I12 step 1; a spot down since step 4): of the profile's pendant role (the pack's tint) at kLookPendantShare
/// of its intensity, under the ceiling near the doorway; none without the pack's pendants or a doorway.
std::optional<json> lookPendantLight(const Room& room, const SceneData& data) {
    if (!data.theme.dressing.pendants) {
        return std::nullopt;
    }
    const auto place = lookPendantPlace(room, data.roomTemplate.doorway);
    if (!place) {
        return std::nullopt;
    }
    const LightStyle& style = data.lighting.pendant;
    // A spot down from the drum (I12 step 4): a point this near the doorway lit the hall's floor and wall beside the
    // doorway through the portal's plane (test_world_floor.cpp's leak probes); its pool is the floor under it.
    return json{{"name", "look:pendant"},
                {"type", "spot"},
                {"position", pointJson(place->first, room.heightMm - kLookPendantBelowCeilingMm, place->second)},
                {"target", pointJson(place->first, 0, place->second)},
                {"outerConeDeg", kLookPendantOuterConeDeg},
                {"innerConeDeg", kLookPendantInnerConeDeg},
                {"color", style.color},
                {"intensity", rounded(style.intensity * kLookPendantShare, 1000.0)},
                {"range", style.range}};
}

/// The gallery (I12 step 2, ADR-0021 §2): its frames stand off the wall as the wall pictures, in front of the paper,
/// at least this far over their fixture's top and this far under the picture rail and a cove (else the cornice or the
/// ceiling),
/// at most this far over the lowest place they could take.
constexpr std::int64_t kGalleryOverFixtureMm = 80;
constexpr std::int64_t kGalleryUnderRailMm = 50;
constexpr std::int64_t kGalleryUnderCeilingMm = 150;
constexpr std::int64_t kGalleryLiftMm = 350;
/// A picture light: its hood (a bar this deep and tall, half the frame's width) this far over the frame and out from
/// the wall on an arm; its light washed on the wall from the hood (the wash's brightest edge, hidden by it) down to the
/// frame's bottom, this much wider than the frame, a few millimetres off the wall (behind the frame, in front of the
/// paper).
constexpr std::int64_t kHoodOverFrameMm = 70;
constexpr std::int64_t kHoodOutMm = 110;
constexpr std::int64_t kHoodDepthMm = 45;
constexpr std::int64_t kHoodHeightMm = 28;
constexpr std::int64_t kHoodArmMm = 10;
constexpr std::int64_t kWashWiderMm = 400;
constexpr std::int64_t kWashOffWallMm = 7;

/// Where a frame over `fixture` hangs: the point of the wall its back stands against (`off` from the wall), none when
/// it stands against no wall.
std::optional<std::pair<std::int64_t, std::int64_t>> againstWall(const Room& room, const Fixture& fixture,
                                                                 std::int64_t off) {
    const auto [backX, backZ] = turned(0, -fixture.depthMm / 2, fixture.yawCdeg);
    const std::int64_t x = fixture.xMm + backX;
    const std::int64_t z = fixture.zMm + backZ;
    if (backX < 0 && x - room.minXMm <= kAgainstWallMm) {
        return std::pair{room.minXMm + off, z};
    }
    if (backX > 0 && room.maxXMm - x <= kAgainstWallMm) {
        return std::pair{room.maxXMm - off, z};
    }
    if (backZ < 0 && z - room.minZMm <= kAgainstWallMm) {
        return std::pair{x, room.minZMm + off};
    }
    if (backZ > 0 && room.maxZMm - z <= kAgainstWallMm) {
        return std::pair{x, room.maxZMm - off};
    }
    return std::nullopt;
}

/// The gallery's frames (I12 step 2, ADR-0021 §2): one over each fixture of the gallery's roles standing against a
/// wall, of the kind its seed draws by the pack's weights, the pack's print in it until the app puts a game's media
/// there, hung in the band between the fixture's top and the picture rail; every other frame a picture light (its
/// hood, its wash on the wall). A frame the band cannot hold is left out. Since I12 step 4 one over each low
/// decoration standing on the floor against a wall too (its drawn piece's top under the tall class; not the kinds the
/// pack's own wall pictures take), each hung over what draws its fixture (a secondary TV over its sideboard).
void addGallery(json& objects, const WorldPlan& plan, const Room& room, const SceneData& data,
                const GalleryDressing& gallery, const RoomChoices& chosen) {
    const RoomDressing& dressing = data.theme.dressing;
    std::int64_t top = room.heightMm - kGalleryUnderCeilingMm;
    if (dressing.shell && !dressing.shell->rail.empty()) {
        top = dressing.shell->railYMm - kPictureRailHeightMm - kGalleryUnderRailMm;
    } else if (dressing.shell && !dressing.shell->cornice.empty()) {
        top = room.heightMm - kCorniceHeightMm - kGalleryUnderCeilingMm;
    }
    if (dressing.cove) {
        top = std::min(top, dressing.cove->yMm - kGalleryUnderRailMm);
    }
    int total = 0;
    for (const int weight : gallery.weights) {
        total += weight;
    }
    if (total <= 0) {
        return;
    }
    std::size_t hung = 0;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != room.id) {
            continue;
        }
        const bool byRole = std::find(gallery.roles.begin(), gallery.roles.end(), fixture.role) != gallery.roles.end();
        const bool pictured =
            dressing.pictures && std::find(dressing.pictures->kinds.begin(), dressing.pictures->kinds.end(),
                                           fixture.prefab.kind) != dressing.pictures->kinds.end();
        const bool lowDecoration = fixture.role == FixtureRole::Decoration && fixture.yMm == 0 && !pictured;
        if (!byRole && !lowDecoration) {
            continue;
        }
        const auto wall = againstWall(room, fixture, kWallPictureOffMm);
        if (!wall) {
            continue;
        }
        const std::int64_t drawnTop = drawnTopMm(plan, room, fixture, data, chosen);
        if (!byRole && drawnTop >= kTallDecorationMm) {
            continue; // a tall piece fills the wall itself
        }
        const std::string local = fixture.id.localText();
        const std::uint64_t seed = core::deriveSeed(room.seed, "gallery", local);
        int draw = static_cast<int>(seed % static_cast<std::uint64_t>(total));
        GalleryKind kind = GalleryKind::Poster;
        for (const GalleryKind candidate : kGalleryKinds) {
            const int weight = gallery.weights[static_cast<std::size_t>(candidate)];
            if (draw < weight) {
                kind = candidate;
                break;
            }
            draw -= weight;
        }
        const GalleryPictureSize picture = galleryPictureSize(kind);
        const std::int64_t width = picture.widthMm + 2 * kGalleryFrameBarMm;
        const std::int64_t height = picture.heightMm + 2 * kGalleryFrameBarMm;
        const std::int64_t low = drawnTop + kGalleryOverFixtureMm;
        if (top - low < height) {
            continue;
        }
        const std::int64_t centre = std::min((low + top) / 2, low + height / 2 + kGalleryLiftMm);
        const std::string id = std::string(kGalleryObjectPrefix) + local;
        const double yaw = degrees(fixture.yawCdeg);
        objects.push_back(
            json{{"id", id},
                 {"prefab", "poster_frame"},
                 {"params", json{{"width", meters(width)},
                                 {"height", meters(height)},
                                 {"frameWidth", meters(kGalleryFrameBarMm)},
                                 {"depth", meters(kGalleryFrameDepthMm)}}},
                 {"position", pointJson(wall->first, centre, wall->second)},
                 {"rotationDeg", vec3Json(0.0, yaw, 0.0)},
                 {"materials", json{{"frame", gallery.frame},
                                    {"picture", kind == GalleryKind::Lightbox ? gallery.lightbox : gallery.picture}}},
                 {"textures", json{{"picture", patternTexture(gallery.pattern, core::deriveSeed(seed, "print"))}}},
                 {"castsShadows", false},
                 {"collides", false}});
        if (!gallery.hood.empty() && kind != GalleryKind::Lightbox && hung % 2 == 0) {
            // In the frame's own axes: x along the wall, z out of it (from the frame's back, kWallPictureOffMm off).
            const auto at = [&](std::int64_t along, std::int64_t out) {
                const auto [dx, dz] = turned(along, out, fixture.yawCdeg);
                return std::pair{wall->first + dx, wall->second + dz};
            };
            const std::int64_t hoodY = centre + height / 2 + kHoodOverFrameMm;
            const auto hood = at(0, kHoodOutMm);
            objects.push_back(dressingPrimitive(
                id + "/hood", "box",
                json{{"size", vec3Json(meters(width / 2), meters(kHoodHeightMm), meters(kHoodDepthMm))}},
                pointJson(hood.first, hoodY, hood.second), yaw, gallery.hood));
            const auto arm = at(0, (kHoodOutMm - kHoodDepthMm / 2 - kWallPictureOffMm) / 2);
            objects.push_back(
                dressingPrimitive(id + "/arm", "box",
                                  json{{"size", vec3Json(meters(kHoodArmMm), meters(kHoodArmMm),
                                                         meters(kHoodOutMm + kWallPictureOffMm - kHoodDepthMm / 2))}},
                                  pointJson(arm.first, hoodY, arm.second), yaw, gallery.hood));
            const std::int64_t washBottom = centre - height / 2;
            const std::int64_t washTop = hoodY;
            const auto wash = at(0, kWashOffWallMm - kWallPictureOffMm);
            objects.push_back(json{
                {"id", id + "/wash"},
                {"primitive", "quad"},
                {"params", json{{"size", json::array({meters(width + kWashWiderMm), meters(washTop - washBottom)})}}},
                {"position",
                 vec3Json(meters(wash.first), static_cast<double>(washBottom + washTop) / 2000.0, meters(wash.second))},
                {"rotationDeg", vec3Json(0.0, yaw, 0.0)},
                {"material", gallery.wash},
                {"castsShadows", false},
                {"collides", false}});
        }
        ++hung;
    }
}

/// The pack's dressing of `room`'s upper walls, ceiling and floor (I6 step 6's review, room_look.hpp's
/// "roomDressing"), in the room's frame: the cove strips, the pictures over the fixtures against a wall, the spots'
/// cans, the sign's pictogram, the runner to the hero corner; since I12 step 1 the room's architecture and pendants.
void addRoomDressing(json& objects, const WorldPlan& plan, const Room& room, const SceneData& data) {
    const RoomDressing& dressing = data.theme.dressing;
    if (const std::optional<ShellDressing>& shell = dressing.shell) {
        addShellDressing(objects, room, data.roomTemplate.doorway, *shell);
    }
    if (const std::optional<PendantDressing>& pendants = dressing.pendants) {
        addPendants(objects, plan, room, data, *pendants);
    }
    if (const std::optional<CoveStrip>& cove = dressing.cove; cove && cove->yMm + kCoveHeightMm < room.heightMm) {
        const std::int64_t length = room.maxZMm - room.minZMm - 2 * kCoveCornerMm;
        const double centreZ = meters((room.minZMm + room.maxZMm) / 2);
        const double y = meters(cove->yMm + kCoveHeightMm / 2);
        const json size = vec3Json(meters(kCoveDepthMm), meters(kCoveHeightMm), meters(length));
        objects.push_back(dressingPrimitive("dressing:cove-w", "box", json{{"size", size}},
                                            vec3Json(meters(room.minXMm + kCoveDepthMm / 2), y, centreZ), 0.0,
                                            cove->west));
        objects.push_back(dressingPrimitive("dressing:cove-e", "box", json{{"size", size}},
                                            vec3Json(meters(room.maxXMm - kCoveDepthMm / 2), y, centreZ), 0.0,
                                            cove->east));
        if (!cove->westWash.empty()) {
            // Each strip's light on its wall: up to the ceiling (the picture turned over: brightest at the strip) and
            // a shorter fall below it, a few millimetres off the wall, facing into the room.
            const std::int64_t top = cove->yMm + kCoveHeightMm;
            const std::int64_t above = room.heightMm - top;
            const std::int64_t below = std::min<std::int64_t>(kCoveWashBelowMm, cove->yMm);
            for (const auto& [suffix, x, yaw, material] :
                 {std::tuple(std::string("w"), room.minXMm + kCoveWashOffMm, 90.0, cove->westWash),
                  std::tuple(std::string("e"), room.maxXMm - kCoveWashOffMm, -90.0, cove->eastWash)}) {
                objects.push_back(json{{"id", "dressing:cove-" + suffix + "/wash-up"},
                                       {"primitive", "quad"},
                                       {"params", json{{"size", json::array({meters(length), meters(above)})}}},
                                       {"position", vec3Json(meters(x), meters(top + above / 2), centreZ)},
                                       {"rotationDeg", vec3Json(0.0, yaw, 180.0)},
                                       {"material", material},
                                       {"castsShadows", false},
                                       {"collides", false}});
                objects.push_back(json{{"id", "dressing:cove-" + suffix + "/wash-down"},
                                       {"primitive", "quad"},
                                       {"params", json{{"size", json::array({meters(length), meters(below)})}}},
                                       {"position", vec3Json(meters(x), meters(cove->yMm - below / 2), centreZ)},
                                       {"rotationDeg", vec3Json(0.0, yaw, 0.0)},
                                       {"material", material},
                                       {"castsShadows", false},
                                       {"collides", false}});
            }
        }
    }
    const RoomChoices chosen = roomChoices(plan, room, data);
    if (const std::optional<GalleryDressing>& gallery = dressing.gallery) {
        addGallery(objects, plan, room, data, *gallery, chosen);
    }
    if (const std::optional<WallPictures>& pictures = dressing.pictures) {
        for (const Fixture& fixture : plan.fixtures) {
            if (fixture.id.room() != room.id) {
                continue;
            }
            const bool byRole =
                std::find(pictures->roles.begin(), pictures->roles.end(), fixture.role) != pictures->roles.end();
            const bool byKind = fixture.role == FixtureRole::Decoration &&
                                std::find(pictures->kinds.begin(), pictures->kinds.end(), fixture.prefab.kind) !=
                                    pictures->kinds.end();
            if (!byRole && !byKind) {
                continue;
            }
            // Its back on a wall: the fixture's local -Z turned with it, half its depth from its pivot.
            const auto [backX, backZ] = turned(0, -fixture.depthMm / 2, fixture.yawCdeg);
            const std::int64_t x = fixture.xMm + backX;
            const std::int64_t z = fixture.zMm + backZ;
            std::int64_t wallX = x;
            std::int64_t wallZ = z;
            if (backX < 0 && x - room.minXMm <= kAgainstWallMm) {
                wallX = room.minXMm + kWallPictureOffMm;
            } else if (backX > 0 && room.maxXMm - x <= kAgainstWallMm) {
                wallX = room.maxXMm - kWallPictureOffMm;
            } else if (backZ < 0 && z - room.minZMm <= kAgainstWallMm) {
                wallZ = room.minZMm + kWallPictureOffMm;
            } else if (backZ > 0 && room.maxZMm - z <= kAgainstWallMm) {
                wallZ = room.maxZMm - kWallPictureOffMm;
            } else {
                continue;
            }
            // Over what draws the fixture (I12 step 4: a secondary TV), never into the ceiling, never over a tall
            // piece.
            const std::int64_t drawnTop = drawnTopMm(plan, room, fixture, data, chosen);
            if (byKind && drawnTop >= kTallDecorationMm) {
                continue;
            }
            const std::int64_t centre = std::max(pictures->centreYMm, drawnTop + 80 + pictures->heightMm / 2);
            if (centre + pictures->heightMm / 2 > room.heightMm - 100) {
                continue;
            }
            const std::uint64_t seed = core::deriveSeed(room.seed, "wall-picture", fixture.id.localText());
            json object{{"id", "dressing:picture:" + fixture.id.localText()},
                        {"prefab", "poster_frame"},
                        {"params", json{{"width", meters(pictures->widthMm)},
                                        {"height", meters(pictures->heightMm)},
                                        {"frameWidth", meters(kWallPictureFrameMm)},
                                        {"depth", meters(kWallPictureDepthMm)}}},
                        {"position", pointJson(wallX, centre, wallZ)},
                        {"rotationDeg", vec3Json(0.0, degrees(fixture.yawCdeg), 0.0)},
                        {"materials", json{{"frame", pictures->frame}, {"picture", pictures->picture}}},
                        {"textures", json{{"picture", patternTexture(pictures->pattern, seed)}}},
                        {"castsShadows", false},
                        {"collides", false}};
            objects.push_back(std::move(object));
        }
    }
    if (const std::optional<SpotCans>& cans = dressing.cans) {
        for (const PlanLight& light : plan.lights) {
            if (light.id.room() != room.id ||
                std::find(cans->roles.begin(), cans->roles.end(), light.role) == cans->roles.end()) {
                continue;
            }
            const std::int64_t bottom = light.yMm + kCanGapMm;
            const std::int64_t top = bottom + kCanHeightMm;
            if (top > room.heightMm) {
                continue;
            }
            const std::string id = "dressing:can:" + light.id.localText();
            objects.push_back(dressingPrimitive(
                id, "cylinder",
                json{{"radius", meters(kCanRadiusMm)}, {"height", meters(kCanHeightMm)}, {"segments", 20}},
                pointJson(light.xMm, (bottom + top) / 2, light.zMm), 0.0, cans->body));
            objects.push_back(dressingPrimitive(
                id + "/lens", "cylinder",
                json{{"radius", meters(kLensRadiusMm)}, {"height", meters(kLensHeightMm)}, {"segments", 20}},
                pointJson(light.xMm, bottom - kLensHeightMm / 2, light.zMm), 0.0, cans->lens));
            if (room.heightMm - top > 5) {
                objects.push_back(dressingPrimitive(
                    id + "/stem", "cylinder",
                    json{{"radius", meters(kStemRadiusMm)}, {"height", meters(room.heightMm - top)}, {"segments", 8}},
                    pointJson(light.xMm, (top + room.heightMm) / 2, light.zMm), 0.0, cans->body));
            }
        }
    }
    if (const std::optional<SignIcon>& icon = dressing.signIcon) {
        // On the sign's right, on its wall at its height, a hand's width from it.
        for (const Fixture& sign : plan.fixtures) {
            if (sign.id.room() != room.id || sign.role != FixtureRole::HeroCorner || sign.id.part() != "sign") {
                continue;
            }
            constexpr std::int64_t kIconGapMm = 100;
            const auto [dx, dz] = turned(sign.widthMm / 2 + kIconGapMm + icon->widthMm / 2, 0, sign.yawCdeg);
            const std::map<std::string, std::string> signMaterials = data.theme.materials("poster_frame", "sign");
            const auto frame = signMaterials.find("frame");
            objects.push_back(json{
                {"id", "dressing:sign-icon"},
                {"prefab", "poster_frame"},
                {"params", json{{"width", meters(icon->widthMm)},
                                {"height", meters(icon->heightMm)},
                                {"frameWidth", 0.015},
                                {"depth", 0.03}}},
                {"position", pointJson(sign.xMm + dx, sign.yMm, sign.zMm + dz)},
                {"rotationDeg", vec3Json(0.0, degrees(sign.yawCdeg), 0.0)},
                {"materials", json{{"frame", frame != signMaterials.end() ? frame->second : icon->material},
                                   {"picture", icon->material}}},
                {"textures", json{{"picture", json{{"pattern", icon->pattern}}}}},
                {"castsShadows", false},
                {"collides", false}});
            break;
        }
    }
    if (const std::optional<HeroRunner>& runner = dressing.runner) {
        // From the back edge of the hero corner's rug (the plan's part, under the hero) out into the room along it,
        // centred on it; a little thinner than a rug, so that a rug drawn over it stays on top.
        for (const Fixture& rug : plan.fixtures) {
            if (rug.id.room() != room.id || rug.role != FixtureRole::HeroCorner || rug.id.part() != "rug") {
                continue;
            }
            const auto [dx, dz] = turned(0, runner->lengthMm / 2 - rug.depthMm / 2, rug.yawCdeg);
            objects.push_back(json{{"id", "dressing:runner"},
                                   {"prefab", "rug"},
                                   {"params", json{{"width", meters(runner->widthMm)},
                                                   {"depth", meters(runner->lengthMm)},
                                                   {"thickness", meters(kRunnerThicknessMm)}}},
                                   {"position", pointJson(rug.xMm + dx, 0, rug.zMm + dz)},
                                   {"rotationDeg", vec3Json(0.0, degrees(rug.yawCdeg), 0.0)},
                                   {"materials", json{{"fabric", runner->material}}},
                                   {"castsShadows", false},
                                   {"collides", false}});
            break;
        }
    }
}

/// The dressed span's object id: "dress:fx:w03/l:2", with ".<first slot>" for a span that does not start at slot 0.
std::string dressingId(const DressedLevel& dressed) {
    std::string id = "dress:" + dressed.level.localText();
    if (dressed.firstSlot != 0) {
        id += "." + std::to_string(dressed.firstSlot);
    }
    return id;
}

} // namespace

const ScenePrefab* ScenePrefabs::find(const PrefabRef& ref) const noexcept {
    const auto it = std::lower_bound(prefabs.begin(), prefabs.end(), ref,
                                     [](const ScenePrefab& prefab, const PrefabRef& key) { return prefab.ref < key; });
    return it != prefabs.end() && it->ref == ref ? &*it : nullptr;
}

std::vector<PrefabRef> scenePrefabRefs(const WorldPlan& plan, const RoomId& room, const MediaFamily& family) {
    std::vector<PrefabRef> refs;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == room) {
            refs.push_back(fixture.prefab);
        }
    }
    for (const DressedLevel& dressed : plan.dressing) {
        if (dressed.level.fixture().room() == room) {
            refs.push_back(dressed.prop);
        }
    }
    refs.push_back(family.item);
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

Expected<ScenePrefabs> loadScenePrefabs(const std::filesystem::path& assetRoot, const assets::PrefabFamilies& families,
                                        std::span<const PrefabRef> refs) {
    ScenePrefabs result;
    for (const PrefabRef& ref : refs) {
        auto resolved = assets::resolvePrefab(assetRoot, families, ref.manifest);
        if (!resolved) {
            Error error = resolved.error();
            error.context = error.context.empty() ? ref.manifest : ref.manifest + ": " + error.context;
            return propagate(std::move(error));
        }
        ScenePrefab prefab;
        prefab.ref = ref;
        prefab.source = resolved->source;
        prefab.fallbackReason = resolved->fallbackReason;
        if (resolved->prefab) {
            if (resolved->generator != ref.kind) {
                return makeError(ErrorCode::ValidationFailed,
                                 fmt::format("resolves to a '{}' prefab, not the '{}' the plan names",
                                             resolved->generator, ref.kind),
                                 ref.manifest);
            }
            prefab.generator = resolved->generator;
            prefab.params = resolved->params.is_object() ? resolved->params : json::object();
            prefab.prefab = std::move(resolved->prefab);
        } else if (resolved->manifest && resolved->manifest->runtime.model) {
            prefab.model = *resolved->manifest->runtime.model;
        } else {
            return makeError(ErrorCode::ValidationFailed, "resolves to neither a procedural prefab nor a model",
                             ref.manifest);
        }
        result.prefabs.push_back(std::move(prefab));
    }
    std::sort(result.prefabs.begin(), result.prefabs.end(),
              [](const ScenePrefab& a, const ScenePrefab& b) { return a.ref < b.ref; });
    result.prefabs.erase(std::unique(result.prefabs.begin(), result.prefabs.end(),
                                     [](const ScenePrefab& a, const ScenePrefab& b) { return a.ref == b.ref; }),
                         result.prefabs.end());
    return result;
}

SceneFurniture resolveSceneFurniture(const std::filesystem::path& assetRoot, const assets::PrefabFamilies& families,
                                     const ThemeFurniture& furniture) {
    const auto resolve = [&](const FurniturePiece& piece) -> std::optional<ScenePiece> {
        auto resolved = assets::resolvePrefab(assetRoot, families, piece.prefab.manifest);
        if (!resolved) {
            spdlog::warn("World: the furniture piece {} does not resolve ({}); its part is drawn as planned",
                         piece.prefab.manifest, resolved.error().toString());
            return std::nullopt;
        }
        if (!resolved->prefab || resolved->generator != piece.prefab.kind) {
            spdlog::warn("World: the furniture piece {} is not a procedural '{}' prefab; its part is drawn as planned",
                         piece.prefab.manifest, piece.prefab.kind);
            return std::nullopt;
        }
        json params = resolved->params.is_object() ? resolved->params : json::object();
        params.merge_patch(piece.params);
        auto built = geometry::buildPrefab(resolved->generator, params);
        if (!built) {
            spdlog::warn("World: the furniture piece {} does not build with its parameters ({}); its part is drawn as "
                         "planned",
                         piece.prefab.manifest, built.error().toString());
            return std::nullopt;
        }
        ScenePiece out;
        out.piece = piece;
        out.prefab.ref = piece.prefab;
        out.prefab.source = resolved->source;
        out.prefab.fallbackReason = resolved->fallbackReason;
        out.prefab.generator = resolved->generator;
        out.prefab.params = std::move(params);
        out.prefab.prefab = std::move(*built);
        return out;
    };
    SceneFurniture result;
    for (const auto& [part, pieces] : furniture.heroParts) {
        if (pieces.empty()) {
            result.heroParts.emplace(part, std::vector<ScenePiece>()); // left out (I6 step 6)
            continue;
        }
        std::vector<ScenePiece> resolved;
        for (const FurniturePiece& piece : pieces) {
            if (auto scenePiece = resolve(piece)) {
                resolved.push_back(std::move(*scenePiece));
            }
        }
        // The CRT's pieces carry the room's video (I6 step 6's hero cabinet): the first holds a ScreenSurface.
        if (part == kHeroTvPart && !resolved.empty() &&
            geometry::findSocket(resolved.front().prefab.prefab->sockets, "ScreenSurface") == nullptr) {
            spdlog::warn("World: the furniture piece {} for the hero corner's crt has no screen surface; the CRT is "
                         "drawn as planned",
                         resolved.front().piece.prefab.manifest);
            resolved.clear();
        }
        if (!resolved.empty()) {
            result.heroParts.emplace(part, std::move(resolved));
        }
    }
    for (const auto& [kind, variants] : furniture.decoration) {
        std::vector<std::pair<std::uint32_t, ScenePiece>> resolved;
        for (const FurnitureVariant& variant : variants) {
            if (auto scenePiece = resolve(variant.piece)) {
                resolved.emplace_back(variant.weightPermille, std::move(*scenePiece));
            }
        }
        if (!resolved.empty()) {
            result.decoration.emplace(kind, std::move(resolved));
        }
    }
    // The hero corner's set (I12 step 3): a piece that fails is left out, the others drawn.
    for (const HeroDressingPiece& dressing : furniture.heroDressing) {
        if (auto scenePiece = resolve(dressing.piece)) {
            result.heroDressing.push_back(
                SceneHeroDressing{dressing.name, dressing.anchor, dressing.prop, std::move(*scenePiece)});
        }
    }
    // The secondary TV (I5 step 3): its screen plays the room's clips, so it must hold one.
    if (furniture.secondaryTv) {
        if (auto scenePiece = resolve(furniture.secondaryTv->piece)) {
            if (geometry::findSocket(scenePiece->prefab.prefab->sockets, "ScreenSurface") != nullptr) {
                result.secondaryTv = SceneSecondaryTv{furniture.secondaryTv->kind, std::move(*scenePiece)};
            } else {
                spdlog::warn("World: the secondary TV {} has no screen surface; no room draws it",
                             furniture.secondaryTv->piece.prefab.manifest);
            }
        }
    }
    return result;
}

std::string chainSignText(std::string_view sign, const RoomId& room) {
    if (!room.isExpansion()) {
        return std::string(sign);
    }
    const std::string number = std::to_string(room.index());
    return sign.empty() ? number : fmt::format("{} {}", sign, number);
}

Expected<SceneData> loadSceneData(const std::filesystem::path& assetRoot, const WorldPlan& plan, const RoomId& roomId,
                                  const std::optional<LightingProfile>& lighting,
                                  std::span<const std::string> platformTags) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    SceneData data;
    auto roomTemplate = loadRoomTemplate(assetRoot, room->templateId);
    if (!roomTemplate) {
        return propagate(roomTemplate.error());
    }
    if (roomTemplate->version != room->templateVersion) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("the room was generated from version {} of its template, the asset root holds "
                                     "version {}",
                                     room->templateVersion, roomTemplate->version),
                         room->templateId);
    }
    data.roomTemplate = std::move(*roomTemplate);
    // The platform of the room's first system (its manifest, or what systems.json gives it, by its platform tags for a
    // system without a row), with the room's recorded family, which is authoritative (ADR-0013 §3), and the theme pack
    // resolved now on the room's floor (ADR-0014 §1: a look, never the plan's record).
    auto building = loadBuildingData(assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    auto systems = loadSystemsData(assetRoot);
    if (!systems) {
        return propagate(systems.error());
    }
    const catalog::SystemName system = room->sections.empty() ? roomId.system() : room->sections.front();
    if (room->kind == RoomKind::Console) {
        auto platform = resolvePlatform(assetRoot, system, platformTags, *systems, *building, room->floor);
        if (!platform) {
            return propagate(platform.error());
        }
        data.platform = std::move(*platform);
    } else {
        // A shared or arcade room stands as its floor's first kind, without a console (ADR-0013 §6).
        data.platform = sharedRoomPlatform(room->kind, room->floor, system, *systems, *building);
    }
    data.platform.mediaFamily = room->mediaFamily;
    // I4b step 8 (ADR-0015 §7): an expansion's signs are its head's with its number.
    data.platform.signText = chainSignText(data.platform.signText, roomId);
    auto registry = loadMediaFamilies(assetRoot);
    if (!registry) {
        return propagate(registry.error());
    }
    const MediaFamily* family = registry->find(room->mediaFamily);
    if (family == nullptr) {
        return makeError(ErrorCode::NotFound, "the media family registry has no such family", room->mediaFamily);
    }
    data.mediaFamily = *family;
    // The template as the room's family stands it (ADR-0013 §4); the plan's fixtures name their prefabs themselves.
    data.roomTemplate = withFamilyFixtures(std::move(data.roomTemplate), data.mediaFamily);
    auto families = assets::loadPrefabFamilies(assetRoot);
    if (!families) {
        return propagate(families.error());
    }
    const std::vector<PrefabRef> refs = scenePrefabRefs(plan, roomId, data.mediaFamily);
    auto prefabs = loadScenePrefabs(assetRoot, *families, refs);
    if (!prefabs) {
        return propagate(prefabs.error());
    }
    data.prefabs = std::move(*prefabs);
    if (auto theme = loadThemeMaterials(assetRoot, data.platform.themePack)) {
        data.theme = std::move(*theme);
    } else {
        spdlog::warn("World: the theme pack \"{}\" does not load ({}); the built-in materials are used",
                     data.platform.themePack, theme.error().toString());
        data.theme = builtInThemeMaterials();
    }
    if (lighting) {
        data.lighting = *lighting;
    } else if (auto profile = loadLightingProfile(assetRoot, kDefaultLightingProfile)) {
        data.lighting = std::move(*profile);
    } else {
        spdlog::warn("World: the lighting profile \"{}\" does not load ({}); the built-in evening profile is used",
                     kDefaultLightingProfile, profile.error().toString());
        data.lighting = eveningLightingProfile();
    }
    // The pack's light: its tints of the profile's roles, the sign's accent in its neon (I6 step 3).
    data.lighting = themedLighting(data.lighting, data.theme);
    // The pack's furniture (I6 step 3b).
    data.furniture = resolveSceneFurniture(assetRoot, *families, data.theme.furniture);
    return data;
}

Expected<SceneBuild> toSceneFile(const WorldPlan& plan, const RoomId& roomId, const SceneData& data) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    SceneBuild build;
    json objects = json::array();

    const room_frames::ShellPlacement shell = room_frames::roomShell(*room, data.roomTemplate);
    objects.push_back(json{{"id", "room"},
                           {"prefab", "room_shell"},
                           {"params", shell.params},
                           {"position", vec3Json(shell.position.x, shell.position.y, shell.position.z)},
                           {"materials", materialsJson(data.theme.materials("room_shell"))}});

    const RoomChoices chosen = roomChoices(plan, *room, data);
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != roomId) {
            continue;
        }
        const ScenePrefab* prefab = data.prefabs.find(fixture.prefab);
        if (prefab == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the fixture's " + fixture.prefab.manifest,
                             fixture.id.toString());
        }
        const std::string heroPart = fixture.role == FixtureRole::HeroCorner ? fixture.id.part() : std::string();
        // The pack's furniture in place of the part or the decoration (I6 step 3b); a part it leaves out is not drawn
        // (I6 step 6).
        if (const std::optional<std::vector<DrawnPiece>> pieces = drawnPieces(plan, *room, fixture, data, chosen)) {
            for (const DrawnPiece& drawn : *pieces) {
                objects.push_back(pieceObject(drawn, fixture, heroPart, data));
            }
            continue;
        }
        objects.push_back(prefabObject(fixture.id.localText(), *prefab, nullptr,
                                       pointJson(fixture.xMm, fixture.yMm, fixture.zMm),
                                       vec3Json(0.0, degrees(fixture.yawCdeg), 0.0), heroPart, data,
                                       fixtureSeed(room->seed, fixture.id)));
    }
    // The hero corner's lived-in set (I12 step 3), after the parts: the props never collide.
    for (const DrawnHeroDressing& dressing : drawnHeroDressing(plan, *room, data, chosen)) {
        json object = pieceObject(dressing.drawn, *dressing.anchor, {}, data);
        if (dressing.dressing->prop) {
            object["collides"] = false;
        }
        objects.push_back(std::move(object));
    }

    for (const DressedLevel& dressed : plan.dressing) {
        if (dressed.level.fixture().room() != roomId) {
            continue;
        }
        const Fixture* fixture = nullptr;
        for (const Fixture& candidate : plan.fixtures) {
            if (candidate.id == dressed.level.fixture()) {
                fixture = &candidate;
                break;
            }
        }
        if (fixture == nullptr) {
            return makeError(ErrorCode::NotFound, "the plan has no fixture of the dressed level",
                             dressed.level.toString());
        }
        const ScenePrefab* fixturePrefab = data.prefabs.find(fixture->prefab);
        const ScenePrefab* prop = data.prefabs.find(dressed.prop);
        if (fixturePrefab == nullptr || prop == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the dressed level or its prop",
                             dressed.level.toString());
        }
        std::string reason;
        auto placed = detail::dressingProp(*room, *fixture, *fixturePrefab, dressed, *prop, reason);
        if (!placed) {
            return propagate(placed.error());
        }
        if (!*placed) {
            // A level too thin for any prop (a display case's leaning level) shows a riser card instead (a counter's
            // a card in each slot, I6 step 6's review: "<span's id>:<slot>", a few covers in turn).
            const bool perSlot = data.theme.dressing.cardPerSlot || fixturePrefab->params.value("counter", false);
            if (const std::vector<detail::RiserCard> cards =
                    detail::riserCards(*fixture, *fixturePrefab, dressed, perSlot);
                !cards.empty()) {
                const std::uint64_t seed = propSeed(room->seed, dressed.level);
                if (!perSlot) {
                    objects.push_back(riserCardObject(dressingId(dressed), cards.front(), data, seed));
                    continue;
                }
                constexpr std::uint64_t kCardCovers = 5;
                for (std::size_t k = 0; k < cards.size(); ++k) {
                    const std::uint64_t slot = dressed.firstSlot + k;
                    objects.push_back(riserCardObject(dressingId(dressed) + ":" + std::to_string(slot), cards[k], data,
                                                      core::deriveSeed(seed, "card", slot % kCardCovers)));
                }
                continue;
            }
            build.undressed.push_back(UndressedSpan{dressed.level, dressed.firstSlot, dressed.count, std::move(reason)});
            continue;
        }
        objects.push_back(prefabObject(dressingId(dressed), *prop, &(*placed)->params, positionOf((*placed)->transform),
                                       rotationOf((*placed)->transform), {}, data,
                                       propSeed(room->seed, dressed.level)));
    }

    // The pack's dressing of the room's upper walls, ceiling and floor (I6 step 6's review).
    addRoomDressing(objects, plan, *room, data);

    json lights = json::array();
    for (const PlanLight& light : plan.lights) {
        if (light.id.room() == roomId) {
            lights.push_back(lightJson(light, data.lighting));
        }
    }
    if (lights.size() > kMaxSceneLights) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("{} lights, more than the {} a scene takes", lights.size(),
                                     kMaxSceneLights),
                         roomId.toString());
    }
    // The profile's accents on the hero corner's parts and the display fixtures, while the scene's limit leaves room
    // for them.
    for (const AccentLight& accent : data.lighting.accents) {
        std::size_t displays = 0; // the display fixtures this accent has lit (its colour alternates on them)
        for (const Fixture& fixture : plan.fixtures) {
            if (lights.size() >= kMaxSceneLights) {
                break;
            }
            if (fixture.id.room() != roomId) {
                continue;
            }
            if (accent.on == AccentAnchor::HeroCorner && fixture.role == FixtureRole::HeroCorner &&
                fixture.id.part() == accent.anchor) {
                lights.push_back(accentJson(accent, fixture));
                break;
            }
            if (accent.on == AccentAnchor::Display && fixture.role == FixtureRole::Display &&
                fixture.prefab.kind == accent.anchor) {
                json light = accentJson(accent, fixture);
                light["name"] = "accent:" + accent.name + ":" + fixture.id.socket();
                if (!accent.colorAlt.empty() && displays % 2 == 1) {
                    light["color"] = accent.colorAlt;
                }
                ++displays;
                lights.push_back(std::move(light));
            }
        }
    }
    // The look pendant's light near the doorway (I12 step 1), last: it takes a slot the plan's lights and the accents
    // leave, never one of theirs (a display case or the featured shelf left unlit, I12 step 1's review); a room whose
    // lights fill the limit shows its drum by its own glow.
    if (std::optional<json> look = lookPendantLight(*room, data); look && lights.size() < kMaxSceneLights) {
        lights.push_back(std::move(*look));
    }

    build.sceneFile = json{{"schemaVersion", scene::kSceneSchemaVersion},
                           {"name", roomId.toString()},
                           {"camera", cameraJson(*room, data.roomTemplate)},
                           {"environment", environmentJson(data.lighting)},
                           {"lights", std::move(lights)},
                           {"objects", std::move(objects)}};
    return build;
}

Expected<std::vector<FixtureId>> secondaryTvFixtures(const WorldPlan& plan, const RoomId& roomId,
                                                     const SceneData& data) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    return chosenSecondaryTvs(plan, *room, data);
}

Expected<std::vector<Fixture>> drawnFixtures(const WorldPlan& plan, const RoomId& roomId, const SceneData& data) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    std::vector<Fixture> fixtures;
    const RoomChoices chosen = roomChoices(plan, *room, data);
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != roomId) {
            continue;
        }
        const std::optional<std::vector<DrawnPiece>> placed = drawnPieces(plan, *room, fixture, data, chosen);
        if (!placed) {
            fixtures.push_back(fixture);
            continue;
        }
        if (placed->empty()) {
            continue; // a part the pack leaves out (I6 step 6) covers nothing
        }
        fixtures.push_back(coveredFixture(fixture, *placed));
    }
    // The hero corner's set (I12 step 3): each piece that collides as a part of the corner of its own,
    // "<socket>.dress-<name>", so that the validator judges the passage and the overlaps piece by piece.
    for (const DrawnHeroDressing& dressing : drawnHeroDressing(plan, *room, data, chosen)) {
        if (dressing.dressing->prop) {
            continue;
        }
        auto id = FixtureId::make(room->id, dressing.anchor->id.socket(),
                                  std::string(kHeroDressingPartPrefix) + dressing.dressing->name);
        if (!id) {
            return propagate(id.error());
        }
        Fixture covered = coveredFixture(*dressing.anchor, {dressing.drawn});
        covered.id = std::move(*id);
        fixtures.push_back(std::move(covered));
    }
    return fixtures;
}

Expected<scene::SceneDescription> toSceneDescription(const WorldPlan& plan, const RoomId& room, const SceneData& data) {
    auto build = toSceneFile(plan, room, data);
    if (!build) {
        return propagate(build.error());
    }
    return scene::parseSceneDescription(build->sceneFile, room.toString());
}

std::string sceneFileText(const nlohmann::json& sceneFile) {
    return sceneFile.dump(2) + "\n";
}

Expected<scene::SceneDescription> shellSceneDescription(const RoomId& roomId, const RoomTemplate& roomTemplate,
                                                        const LightingProfile& lighting) {
    const Footprint interior = interiorFootprint(roomTemplate, 0);
    Room room;
    room.id = roomId;
    room.templateId = roomTemplate.id;
    room.templateVersion = roomTemplate.version;
    room.minXMm = interior.minXMm;
    room.maxXMm = interior.maxXMm;
    room.minZMm = interior.minZMm;
    room.maxZMm = interior.maxZMm;
    room.heightMm = roomTemplate.heightMm;
    WorldPlan plan;
    // The pendant every room has (I3 step 3: over the middle of the base room, 1.1 m below the ceiling), so that the
    // empty room reads as a room and not as a dark box.
    constexpr std::int64_t kShellPendantBelowCeilingMm = 1100;
    auto pendant = FixtureId::make(roomId, "room", "pendant");
    if (!pendant) {
        return propagate(pendant.error());
    }
    PlanLight light;
    light.id = *pendant;
    light.role = LightRole::Pendant;
    light.xMm = interior.minXMm + (interior.maxXMm - interior.minXMm) / 2;
    light.yMm = roomTemplate.heightMm - kShellPendantBelowCeilingMm;
    light.zMm = interior.minZMm + (interior.maxZMm - interior.minZMm) / 2;
    light.targetXMm = light.xMm;
    light.targetYMm = light.yMm;
    light.targetZMm = light.zMm;
    plan.lights.push_back(light);
    plan.rooms.push_back(std::move(room));
    SceneData data;
    data.roomTemplate = roomTemplate;
    data.lighting = lighting;
    return toSceneDescription(plan, roomId, data);
}

DoorwayCamera doorwayCamera(const Room& room, const RoomTemplate& roomTemplate) noexcept {
    const Doorway& door = roomTemplate.doorway;
    DoorwayCamera camera;
    camera.eyeY = kEyeHeight;
    camera.pitchDeg = kCameraPitchDeg;
    switch (door.wall) {
    case Wall::North:
        camera.x = meters(door.offsetMm);
        camera.z = meters(room.minZMm) + kCameraInsideDoorway;
        camera.yawDeg = 180.0;
        break;
    case Wall::East:
        camera.x = meters(room.maxXMm) - kCameraInsideDoorway;
        camera.z = meters(door.offsetMm);
        camera.yawDeg = -90.0;
        break;
    case Wall::West:
        camera.x = meters(room.minXMm) + kCameraInsideDoorway;
        camera.z = meters(door.offsetMm);
        camera.yawDeg = 90.0;
        break;
    case Wall::South:
    case Wall::None:
        camera.x = meters(door.offsetMm);
        camera.z = meters(room.maxZMm) - kCameraInsideDoorway;
        break;
    }
    return camera;
}

namespace room_frames {

glm::mat4 yawRotation(std::int32_t yawCdeg) noexcept {
    const std::int32_t normalized = ((yawCdeg % 36000) + 36000) % 36000;
    glm::mat4 rotation(1.0f);
    float c = 1.0f;
    float s = 0.0f;
    switch (normalized) {
    case 0:
        break;
    case 9000:
        c = 0.0f;
        s = 1.0f;
        break;
    case 18000:
        c = -1.0f;
        s = 0.0f;
        break;
    case 27000:
        c = 0.0f;
        s = -1.0f;
        break;
    default: {
        const double radians = static_cast<double>(yawCdeg) / 100.0 * (3.14159265358979323846 / 180.0);
        c = static_cast<float>(std::cos(radians));
        s = static_cast<float>(std::sin(radians));
        break;
    }
    }
    // glm::rotate about +Y: x' = c x + s z, z' = -s x + c z (counter-clockwise seen from above).
    rotation[0] = glm::vec4(c, 0.0f, -s, 0.0f);
    rotation[2] = glm::vec4(s, 0.0f, c, 0.0f);
    return rotation;
}

glm::mat4 fixtureTransform(const Fixture& fixture) noexcept {
    glm::mat4 transform = yawRotation(fixture.yawCdeg);
    transform[3] = glm::vec4(static_cast<float>(meters(fixture.xMm)), static_cast<float>(meters(fixture.yMm)),
                             static_cast<float>(meters(fixture.zMm)), 1.0f);
    return transform;
}

ShellPlacement roomShell(const Room& room, const RoomTemplate& roomTemplate) {
    ShellPlacement shell;
    const std::int64_t centreXMm = (room.minXMm + room.maxXMm) / 2;
    const std::int64_t centreZMm = (room.minZMm + room.maxZMm) / 2;
    shell.position = glm::vec3(static_cast<float>(meters(centreXMm)), 0.0f, static_cast<float>(meters(centreZMm)));
    json doorway = nullptr;
    const Doorway& door = roomTemplate.doorway;
    if (door.wall != Wall::None) {
        // The template's offset is along +X from the base room's centre for the north and south walls, along +Z for
        // the east and west walls; the shell's is from its own wall's centre.
        const std::int64_t offsetMm =
            door.wall == Wall::North || door.wall == Wall::South ? door.offsetMm - centreXMm : door.offsetMm - centreZMm;
        doorway = json{{"wall", std::string(toString(door.wall))},
                       {"offset", meters(offsetMm)},
                       {"width", meters(door.widthMm)},
                       {"height", meters(door.heightMm)}};
    }
    shell.params = json{{"size", json::array({meters(room.maxXMm - room.minXMm), meters(room.maxZMm - room.minZMm)})},
                        {"wallHeight", meters(room.heightMm)},
                        {"wallThickness", meters(roomTemplate.wallThicknessMm)},
                        {"metersPerTile", kShellMetersPerTile},
                        {"ceiling", true},
                        {"baseboardHeight", kShellBaseboard},
                        {"wainscotHeight", kShellWainscot},
                        {"doorway", std::move(doorway)}};
    return shell;
}

} // namespace room_frames

namespace detail {

const geometry::Socket* slotSocket(const geometry::ProceduralPrefab& prefab, std::uint32_t level,
                                   std::uint32_t index) noexcept {
    for (const geometry::Socket& socket : prefab.sockets) {
        if (socket.kind != geometry::SocketKind::ShelfSlot) {
            continue;
        }
        const auto address = geometry::parseSlotId(socket.id);
        if (address && static_cast<std::uint32_t>(address->level) == level &&
            static_cast<std::uint32_t>(address->index) == index) {
            return &socket;
        }
    }
    return nullptr;
}

const geometry::Socket* levelSocket(const geometry::ProceduralPrefab& prefab, std::uint32_t level) noexcept {
    for (const geometry::Socket& socket : prefab.sockets) {
        const auto parsed = geometry::parseLevelId(socket.id);
        if (parsed && static_cast<std::uint32_t>(*parsed) == level) {
            return &socket;
        }
    }
    return nullptr;
}

std::uint32_t slotCount(const geometry::ProceduralPrefab& prefab, std::uint32_t level) noexcept {
    std::uint32_t count = 0;
    for (const geometry::Socket& socket : prefab.sockets) {
        if (socket.kind != geometry::SocketKind::ShelfSlot) {
            continue;
        }
        const auto address = geometry::parseSlotId(socket.id);
        if (address && static_cast<std::uint32_t>(address->level) == level) {
            ++count;
        }
    }
    return count;
}

Expected<std::optional<DressingProp>> dressingProp(const Room& room, const Fixture& fixture,
                                                   const ScenePrefab& fixturePrefab, const DressedLevel& dressed,
                                                   const ScenePrefab& prop, std::string& reason) {
    if (!fixturePrefab.prefab) {
        reason = "the fixture is a model without level sockets";
        return std::optional<DressingProp>{};
    }
    if (!prop.prefab) {
        reason = "the prop is a model, which the dressing cannot size";
        return std::optional<DressingProp>{};
    }
    const geometry::ProceduralPrefab& built = *fixturePrefab.prefab;
    const std::uint32_t levelNumber = dressed.level.level();
    const geometry::Socket* level = levelSocket(built, levelNumber);
    const std::uint32_t slots = slotCount(built, levelNumber);
    if (level == nullptr || dressed.count == 0 || dressed.firstSlot + dressed.count > slots) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("the fixture's prefab has no level {} holding slots {} to {}", levelNumber,
                                     dressed.firstSlot, dressed.firstSlot + dressed.count),
                         dressed.level.toString());
    }
    const core::Aabb& volume = level->bounds;
    float left = volume.min.x;
    float right = volume.max.x;
    if (dressed.firstSlot != 0 || dressed.count != slots) {
        // A part of the level: from the first slot's left edge to the last one's right edge, in the level's frame.
        const glm::mat4 toLevel = glm::inverse(level->transform);
        const geometry::Socket* first = slotSocket(built, levelNumber, dressed.firstSlot);
        const geometry::Socket* last = slotSocket(built, levelNumber, dressed.firstSlot + dressed.count - 1);
        if (first == nullptr || last == nullptr) {
            return makeError(ErrorCode::ValidationFailed, "the fixture's prefab lacks a slot of the dressed span",
                             dressed.level.toString());
        }
        const float firstX = (toLevel * first->transform)[3].x;
        const float lastX = (toLevel * last->transform)[3].x;
        left = std::max(volume.min.x, firstX + first->bounds.min.x);
        right = std::min(volume.max.x, lastX + last->bounds.max.x);
    }
    const std::int64_t widthMm = toMm(right - left);
    const std::int64_t heightMm = toMm(volume.size().y);
    const std::int64_t depthMm = toMm(volume.size().z);
    if (widthMm < kDressingMinWidthMm || widthMm > kDressingMaxWidthMm || heightMm < kDressingMinSideMm ||
        heightMm > kDressingMaxSideMm || depthMm < kDressingMinSideMm || depthMm > kDressingMaxSideMm) {
        reason = fmt::format("its free volume, {} x {} x {} mm, is outside what a {} prop takes", widthMm, heightMm,
                             depthMm, prop.generator);
        return std::optional<DressingProp>{};
    }
    DressingProp result;
    result.generator = prop.generator;
    result.params = prop.params;
    result.params["width"] = meters(widthMm);
    result.params["height"] = meters(heightMm);
    result.params["depth"] = meters(depthMm);
    result.params["seed"] = static_cast<std::int64_t>(propSeed(room.seed, dressed.level) & 0x7FFFFFFFu);
    result.size = glm::vec3(static_cast<float>(meters(widthMm)), static_cast<float>(meters(heightMm)),
                            static_cast<float>(meters(depthMm)));
    // Pivot floor in the middle of the span, on the level's surface, centred in its depth.
    const float centreX = static_cast<float>(meters(toMm(0.5f * (left + right))));
    const float centreZ = static_cast<float>(meters(toMm(volume.center().z)));
    const float floorY = static_cast<float>(meters(toMm(volume.min.y)));
    result.transform = room_frames::fixtureTransform(fixture) * level->transform *
                       glm::translate(glm::mat4(1.0f), glm::vec3(centreX, floorY, centreZ));
    return std::optional<DressingProp>{std::move(result)};
}

std::optional<RiserCard> riserCard(const Fixture& fixture, const ScenePrefab& fixturePrefab,
                                   const DressedLevel& dressed) {
    if (!fixturePrefab.prefab || fixturePrefab.generator != "display_case") {
        return std::nullopt;
    }
    const geometry::ProceduralPrefab& built = *fixturePrefab.prefab;
    const std::uint32_t levelNumber = dressed.level.level();
    const geometry::Socket* level = levelSocket(built, levelNumber);
    const std::uint32_t slots = slotCount(built, levelNumber);
    if (level == nullptr || dressed.count == 0 || dressed.firstSlot + dressed.count > slots) {
        return std::nullopt;
    }
    const geometry::Socket* first = slotSocket(built, levelNumber, dressed.firstSlot);
    const geometry::Socket* last = slotSocket(built, levelNumber, dressed.firstSlot + dressed.count - 1);
    if (first == nullptr || last == nullptr) {
        return std::nullopt;
    }
    // The card spans the slots' width less a margin and stands as tall as their items, leaning on the level's panel
    // like the items it stands in for; a card no wider than kMinCardWidthMm is not worth showing.
    const glm::mat4 toLevel = glm::inverse(level->transform);
    const glm::mat4 firstFrame = toLevel * first->transform;
    const glm::mat4 lastFrame = toLevel * last->transform;
    const float left = firstFrame[3].x + first->bounds.min.x;
    const float right = lastFrame[3].x + last->bounds.max.x;
    constexpr float kMarginM = 0.01f;
    constexpr std::int64_t kMinCardWidthMm = 60;
    constexpr std::int64_t kCardDepthMm = 5; // poster_frame's thinnest
    constexpr std::int64_t kCardFrameMm = 5;
    const std::int64_t widthMm = toMm(right - left - 2.0f * kMarginM);
    const std::int64_t heightMm = toMm(first->bounds.size().y);
    if (widthMm < kMinCardWidthMm || heightMm < 3 * kCardFrameMm) {
        return std::nullopt;
    }
    RiserCard card;
    card.params = json{{"width", meters(widthMm)},
                       {"height", meters(heightMm)},
                       {"frameWidth", meters(kCardFrameMm)},
                       {"depth", meters(kCardDepthMm)}};
    // The poster frame's pivot is its back's centre: on the first slot's back plane, half the card up from its floor.
    const glm::vec3 slotBack = glm::vec3(firstFrame * glm::vec4(0.0f, 0.0f, first->bounds.min.z, 1.0f));
    const float centreX = static_cast<float>(meters(toMm(0.5f * (left + right))));
    const glm::mat4 slotInLevel = firstFrame;
    glm::mat4 frame = slotInLevel;
    frame[3] = glm::vec4(centreX, slotBack.y, slotBack.z, 1.0f);
    card.transform = room_frames::fixtureTransform(fixture) * level->transform * frame *
                     glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, static_cast<float>(meters(heightMm)) * 0.5f, 0.0f));
    return card;
}

std::vector<RiserCard> riserCards(const Fixture& fixture, const ScenePrefab& fixturePrefab,
                                  const DressedLevel& dressed, bool perSlot) {
    std::vector<RiserCard> cards;
    if (!fixturePrefab.prefab || fixturePrefab.generator != "display_case" ||
        !(perSlot || fixturePrefab.params.value("counter", false))) {
        if (auto card = riserCard(fixture, fixturePrefab, dressed)) {
            cards.push_back(std::move(*card));
        }
        return cards;
    }
    const geometry::ProceduralPrefab& built = *fixturePrefab.prefab;
    const std::uint32_t levelNumber = dressed.level.level();
    const geometry::Socket* level = levelSocket(built, levelNumber);
    if (level == nullptr || dressed.count == 0 || dressed.firstSlot + dressed.count > slotCount(built, levelNumber)) {
        return cards;
    }
    constexpr std::int64_t kCardInsetMm = 3;
    constexpr std::int64_t kCardDepthMm = 5;
    constexpr std::int64_t kCardFrameMm = 5; // poster_frame's thinnest frame
    const glm::mat4 toLevel = glm::inverse(level->transform);
    for (std::uint32_t k = 0; k < dressed.count; ++k) {
        const geometry::Socket* slot = slotSocket(built, levelNumber, dressed.firstSlot + k);
        if (slot == nullptr) {
            return {};
        }
        // A card the item's size less its inset, standing on the slot's back plane like the item it stands in for.
        const std::int64_t widthMm = toMm(slot->bounds.size().x) - 2 * kCardInsetMm;
        const std::int64_t heightMm = toMm(slot->bounds.size().y) - kCardInsetMm;
        if (widthMm < 4 * kCardFrameMm || heightMm < 4 * kCardFrameMm) {
            return {};
        }
        const glm::mat4 slotFrame = toLevel * slot->transform;
        const glm::vec3 slotBack = glm::vec3(slotFrame * glm::vec4(0.0f, 0.0f, slot->bounds.min.z, 1.0f));
        glm::mat4 frame = slotFrame;
        frame[3] = glm::vec4(slotBack, 1.0f);
        RiserCard card;
        card.params = json{{"width", meters(widthMm)},
                           {"height", meters(heightMm)},
                           {"frameWidth", meters(kCardFrameMm)},
                           {"depth", meters(kCardDepthMm)}};
        card.transform =
            room_frames::fixtureTransform(fixture) * level->transform * frame *
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, static_cast<float>(meters(heightMm)) * 0.5f, 0.0f));
        cards.push_back(std::move(card));
    }
    return cards;
}

} // namespace detail

} // namespace rv::world
