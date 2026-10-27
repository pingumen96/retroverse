#include <rv/world/scene_builder.hpp>

#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>

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
    if (kind == "crt") {
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
};

/// The pieces that draw `fixture` (scene_builder.hpp's file comment), before the footprint guard; nothing: the plan's
/// prefab draws it; none: it is not drawn (a hero part the pack leaves out, I6 step 6).
std::optional<std::vector<DrawnPiece>> placedPieces(const Room& room, const Fixture& fixture, const SceneData& data) {
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
    const auto found = data.furniture.decoration.find(fixture.prefab.kind);
    const ScenePrefab* own = data.prefabs.find(fixture.prefab);
    if (found == data.furniture.decoration.end() || own == nullptr || !own->prefab) {
        return std::nullopt;
    }
    std::uint64_t total = 0;
    for (const auto& [weight, piece] : found->second) {
        total += weight;
    }
    if (total == 0) {
        return std::nullopt;
    }
    // The variant by weight, from the room's seed and the fixture's id.
    core::Random random(core::deriveSeed(room.seed, "furniture", fixture.id.localText()));
    std::uint64_t draw = random.below(total);
    const ScenePiece* chosen = nullptr;
    for (const auto& [weight, piece] : found->second) {
        if (draw < weight) {
            chosen = &piece;
            break;
        }
        draw -= weight;
    }
    // Its back on the socket's back edge and centred across it, where the plan's prefab stands.
    const core::Aabb& planned = own->prefab->bounds;
    const core::Aabb& drawnBounds = chosen->prefab.prefab->bounds;
    const std::int64_t dx =
        toMm(0.5f * (planned.min.x + planned.max.x)) - toMm(0.5f * (drawnBounds.min.x + drawnBounds.max.x));
    const std::int64_t dz = toMm(planned.min.z) - toMm(drawnBounds.min.z);
    const auto [roomX, roomZ] = turned(dx, dz, fixture.yawCdeg);
    DrawnPiece drawn;
    drawn.piece = chosen;
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
                                                   const SceneData& data);

/// Whether the hero corner of `fixture`'s socket draws its CRT as the pack's pieces (resolved, holding the screen,
/// inside the guard): only then are the parts the pack leaves out left out, the cabinet standing in their place (I6
/// step 6's review: else the CRT, drawn as planned, would float where its TV cabinet is missing).
bool heroCrtSubstituted(const WorldPlan& plan, const Room& room, const Fixture& fixture, const SceneData& data) {
    for (const Fixture& candidate : plan.fixtures) {
        if (candidate.role == FixtureRole::HeroCorner && candidate.id.room() == room.id &&
            candidate.id.socket() == fixture.id.socket() && candidate.id.part() == kHeroTvPart) {
            const std::optional<std::vector<DrawnPiece>> pieces = drawnPieces(plan, room, candidate, data);
            return pieces.has_value() && !pieces->empty();
        }
    }
    return false;
}

/// The pieces that draw `fixture` (scene_builder.hpp's file comment); nothing: the plan's prefab draws it; none: it is
/// not drawn (I6 step 6: a part the pack leaves out, while its CRT stands as the pack's cabinet). The guard of ADR-0014
/// §1 (amended): a decoration's variant that leaves its socket's footprint (turned with the socket), or a hero part's
/// piece that leaves the hero corner (and the planned part's own floor, where the part reaches out of the corner: the
/// lamp beside the TV), is not drawn; the plan's prefab is, with a warning logged once.
std::optional<std::vector<DrawnPiece>> drawnPieces(const WorldPlan& plan, const Room& room, const Fixture& fixture,
                                                   const SceneData& data) {
    std::optional<std::vector<DrawnPiece>> placed = placedPieces(room, fixture, data);
    if (placed && placed->empty() && !heroCrtSubstituted(plan, room, fixture, data)) {
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
    }
    return placed;
}

/// A piece's object: its prefab with its merged parameters, the pack's materials of its kind (the part's own when it
/// is of the part's kind) under its own, the kind's pictures under its patterns.
json pieceObject(const DrawnPiece& drawn, const Fixture& fixture, std::string_view heroPart, const SceneData& data) {
    const ScenePiece& piece = *drawn.piece;
    const ScenePrefab& prefab = piece.prefab;
    std::map<std::string, std::string> materials = piece.piece.prefab.kind == fixture.prefab.kind
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

/// The pack's dressing of `room`'s upper walls, ceiling and floor (I6 step 6's review, room_look.hpp's
/// "roomDressing"), in the room's frame: the cove strips, the pictures over the fixtures against a wall, the spots'
/// cans, the sign's pictogram, the runner to the hero corner.
void addRoomDressing(json& objects, const WorldPlan& plan, const Room& room, const SceneData& data) {
    const RoomDressing& dressing = data.theme.dressing;
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
            // Over the fixture, never into the ceiling.
            const std::int64_t centre = std::max(pictures->centreYMm, fixture.yMm + fixture.heightMm + 80 +
                                                                          pictures->heightMm / 2);
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
    return result;
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
        if (const std::optional<std::vector<DrawnPiece>> pieces = drawnPieces(plan, *room, fixture, data)) {
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

    build.sceneFile = json{{"schemaVersion", scene::kSceneSchemaVersion},
                           {"name", roomId.toString()},
                           {"camera", cameraJson(*room, data.roomTemplate)},
                           {"environment", environmentJson(data.lighting)},
                           {"lights", std::move(lights)},
                           {"objects", std::move(objects)}};
    return build;
}

Expected<std::vector<Fixture>> drawnFixtures(const WorldPlan& plan, const RoomId& roomId, const SceneData& data) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    std::vector<Fixture> fixtures;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != roomId) {
            continue;
        }
        const std::optional<std::vector<DrawnPiece>> placed = drawnPieces(plan, *room, fixture, data);
        if (!placed) {
            fixtures.push_back(fixture);
            continue;
        }
        if (placed->empty()) {
            continue; // a part the pack leaves out (I6 step 6) covers nothing
        }
        const std::vector<DrawnPiece>& pieces = *placed;
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
