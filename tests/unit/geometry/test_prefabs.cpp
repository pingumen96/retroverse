#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>

#include "mesh_checks.hpp"

using Catch::Approx;
using nlohmann::json;
using rv::core::Aabb;
using rv::geometry::MeshData;
using rv::geometry::PrefabPart;
using rv::geometry::ProceduralPrefab;
using rv::geometry::Socket;
using rv::geometry::SocketKind;

namespace {

ProceduralPrefab buildOk(std::string_view kind, const json& params = nullptr) {
    auto prefab = rv::geometry::buildPrefab(kind, params);
    if (!prefab) {
        FAIL(std::string(kind) + " " + params.dump() + ": " + prefab.error().toString());
    }
    return std::move(*prefab);
}

rv::Error buildError(std::string_view kind, const json& params) {
    auto prefab = rv::geometry::buildPrefab(kind, params);
    INFO(std::string(kind) + " " + params.dump());
    REQUIRE_FALSE(prefab.has_value());
    CHECK_FALSE(prefab.error().message.empty());
    return prefab.error();
}

const PrefabPart& part(const ProceduralPrefab& prefab, std::string_view role) {
    const PrefabPart* found = rv::geometry::findPart(prefab, role);
    INFO(prefab.kind << " role " << role);
    REQUIRE(found != nullptr);
    return *found;
}

const Socket& socket(const ProceduralPrefab& prefab, std::string_view id) {
    const Socket* found = rv::geometry::findSocket(prefab.sockets, id);
    INFO(prefab.kind << " socket " << id);
    REQUIRE(found != nullptr);
    return *found;
}

glm::vec3 position(const Socket& s) {
    return glm::vec3(s.transform[3]);
}

glm::vec3 axis(const Socket& s, int column) {
    return glm::vec3(s.transform[column]);
}

std::vector<const MeshData*> meshes(const ProceduralPrefab& prefab) {
    std::vector<const MeshData*> list;
    for (const PrefabPart& p : prefab.parts) {
        list.push_back(&p.mesh);
    }
    return list;
}

std::size_t rayHits(const ProceduralPrefab& prefab, const rv::core::Ray& ray, float maxDistance) {
    std::size_t hits = 0;
    for (const PrefabPart& p : prefab.parts) {
        hits += rv::test::rayHits(p.mesh, ray, maxDistance);
    }
    return hits;
}

/// Distance to the closest surface along `ray` (unit direction), or +inf.
float firstHit(const ProceduralPrefab& prefab, const rv::core::Ray& ray) {
    float best = std::numeric_limits<float>::infinity();
    for (const PrefabPart& p : prefab.parts) {
        for (std::size_t t = 0; t < p.mesh.triangleCount(); ++t) {
            const rv::test::TriangleRef tri = rv::test::triangleAt(p.mesh, t);
            if (const auto hit = rv::test::intersect(ray, tri.v0.position, tri.v1.position, tri.v2.position)) {
                best = std::min(best, *hit);
            }
        }
    }
    return best;
}

bool closeTo(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f) {
    return rv::core::nearlyEqual(a, b, epsilon);
}

bool closeTo(const Aabb& a, const Aabb& b, float epsilon = 1e-5f) {
    return closeTo(a.min, b.min, epsilon) && closeTo(a.max, b.max, epsilon);
}

/// Every invariant a procedural prefab must satisfy, whatever its parameters.
void checkPrefab(const ProceduralPrefab& prefab) {
    INFO(prefab.kind);
    REQUIRE_FALSE(prefab.parts.empty());
    Aabb unionOfParts;
    std::set<std::string> roles;
    for (const PrefabPart& p : prefab.parts) {
        INFO(p.mesh.name);
        CHECK(roles.insert(p.materialRole).second);
        CHECK(p.mesh.name == prefab.kind + "/" + p.materialRole);
        CHECK_FALSE(p.mesh.empty());
        CHECK(rv::geometry::validateMesh(p.mesh).has_value());
        CHECK(rv::test::nonUnitNormals(p.mesh) == 0);
        CHECK(rv::test::windingMismatches(p.mesh) == 0);
        CHECK(rv::test::badTangents(p.mesh) == 0);
        CHECK(rv::test::tangentFrameMismatches(p.mesh) == 0);
        unionOfParts.expand(p.mesh.bounds);
    }
    CHECK(prefab.bounds == unionOfParts);
    std::set<std::string> ids;
    for (const Socket& s : prefab.sockets) {
        INFO(s.id);
        CHECK(ids.insert(s.id).second);
        CHECK(s.nodeName == (s.kind == SocketKind::Anchor ? "Anchor_" + s.id : s.id));
        CHECK(rv::core::isFinite(s.transform));
        // Rigid frame: orthonormal right-handed rotation plus translation.
        const glm::mat3 rotation(s.transform);
        CHECK(closeTo(glm::transpose(rotation) * rotation[0], glm::vec3(1.0f, 0.0f, 0.0f)));
        CHECK(closeTo(glm::transpose(rotation) * rotation[1], glm::vec3(0.0f, 1.0f, 0.0f)));
        CHECK(glm::determinant(rotation) == Approx(1.0f));
        CHECK(s.transform[0][3] == 0.0f);
        CHECK(s.transform[3][3] == 1.0f);
        // Exactly the mesh-backed sockets name a part.
        CHECK(s.meshIndex.has_value() == (s.kind == SocketKind::ScreenSurface || s.kind == SocketKind::ColliderProxy));
        if (s.meshIndex) {
            CHECK(*s.meshIndex < prefab.parts.size());
        }
    }
    // Colliders: solid, inside the prefab, uniquely named when there are several.
    std::set<std::string> colliderNames;
    for (const rv::geometry::PrefabCollider& collider : prefab.colliders) {
        INFO("collider " << collider.name);
        CHECK_FALSE(collider.box.isEmpty());
        CHECK(prefab.bounds.inflated(1e-5f).contains(collider.box));
        CHECK(colliderNames.insert(collider.name).second);
    }
}

const std::vector<std::string_view> kExpectedKinds{
    "room_shell", "shelf",        "cartridge",  "crt",         "tv_cabinet",     "console_cartridge", "poster_frame",
    "rug",        "floor_lamp",   "armchair",   "display_case", "wall_shelf",    "storage_box",       "magazine_stack",
    "bookends",   "plant",        "sofa",       "beanbag",     "desk",           "office_chair",      "bar_stool",
    "banker_lamp", "showcase",    "column",     "railing",     "hub_landmark",   "info_totem",        "arcade_cabinet",
    "secondary_tv"};

} // namespace

TEST_CASE("geometry/prefabs: kind lists are complete and ordered", "[geometry][prefabs]") {
    const auto kinds = rv::geometry::prefabKinds();
    CHECK(std::vector<std::string_view>(kinds.begin(), kinds.end()) == kExpectedKinds);
    const auto primitives = rv::geometry::primitiveKinds();
    CHECK(std::vector<std::string_view>(primitives.begin(), primitives.end()) ==
          std::vector<std::string_view>{"box", "plane", "quad", "sphere", "cylinder"});
}

TEST_CASE("geometry/prefabs: every kind builds with defaults into valid parts", "[geometry][prefabs]") {
    for (const std::string_view kind : rv::geometry::prefabKinds()) {
        for (const json& params : {json(nullptr), json::object()}) {
            const ProceduralPrefab prefab = buildOk(kind, params);
            CHECK(prefab.kind == kind);
            checkPrefab(prefab);
        }
    }
}

TEST_CASE("geometry/prefabs: every contract role exists, in contract order", "[geometry][prefabs][roles]") {
    const std::map<std::string_view, std::vector<std::string>> expected{
        {"room_shell", {"floor", "wall", "ceiling", "trim"}},
        {"shelf", {"wood_primary", "wood_secondary"}},
        {"cartridge", {"plastic_shell", "label"}},
        {"crt", {"plastic_body", "plastic_bezel", "screen", "plastic_dark"}},
        {"tv_cabinet", {"wood_primary", "wood_secondary", "metal_trim"}},
        {"console_cartridge", {"plastic_body", "plastic_dark", "plastic_accent"}},
        {"poster_frame", {"frame", "picture"}},
        {"rug", {"fabric"}},
        {"floor_lamp", {"metal", "lamp_shade"}},
        {"armchair", {"fabric", "wood"}},
        {"display_case", {"wood_primary", "wood_secondary", "metal_trim", "fabric", "glass"}},
        {"wall_shelf", {"wood_primary", "wood_secondary"}},
        {"storage_box", {"cardboard", "label"}},
        {"magazine_stack", {"paper", "cover"}},
        {"bookends", {"metal", "book", "book_alt"}},
        {"plant", {"pot", "soil", "leaves", "leaves_alt"}},
        {"sofa", {"fabric", "wood", "pillow"}},
        {"beanbag", {"fabric"}},
        {"desk", {"wood_primary", "wood_secondary", "metal_trim", "plastic_body", "plastic_dark", "screen"}},
        {"office_chair", {"fabric", "plastic_dark", "metal"}},
        {"bar_stool", {"fabric", "metal"}},
        {"banker_lamp", {"wood_primary", "wood_secondary", "metal", "lamp_shade"}},
        {"showcase",
         {"wood_primary", "wood_secondary", "metal_trim", "fabric", "glass", "plastic_body", "plastic_dark",
          "plastic_accent"}},
        // I6 step 5's hub.
        {"column", {"wood_primary", "wood_secondary", "metal_trim"}},
        {"railing", {"wood_primary", "wood_secondary", "metal"}},
        {"hub_landmark",
         {"wood_primary", "wood_secondary", "metal_trim", "glass", "fabric", "plastic_body", "plastic_dark",
          "plastic_accent", "globe", "globe_core"}},
        {"info_totem", {"wood_primary", "wood_secondary", "metal_trim", "screen", "sign"}},
        // I6 step 6's arcade cabinets: a pair, the second in the alternate paint, moulding and marquee.
        {"arcade_cabinet",
         {"body", "body_alt", "front", "molding", "molding_alt", "plastic_dark", "metal_trim", "plastic_accent",
          "marquee", "marquee_alt", "screen_back"}},
        // I5 step 3's secondary TV: the sideboard's roles, then the CRT's.
        {"secondary_tv",
         {"wood_primary", "wood_secondary", "metal_trim", "plastic_body", "plastic_bezel", "screen", "plastic_dark"}},
    };
    REQUIRE(expected.size() == kExpectedKinds.size());
    for (const auto& [kind, roles] : expected) {
        const ProceduralPrefab prefab = buildOk(kind);
        std::vector<std::string> actual;
        for (const PrefabPart& p : prefab.parts) {
            actual.push_back(p.materialRole);
        }
        INFO(kind);
        CHECK(actual == roles);
    }
    // Optional parts disappear with their geometry.
    CHECK(rv::geometry::findPart(buildOk("shelf", {{"back", false}}), "wood_secondary") == nullptr);
    CHECK(rv::geometry::findPart(buildOk("room_shell", {{"ceiling", false}}), "ceiling") == nullptr);
    CHECK(rv::geometry::findPart(buildOk("room_shell", {{"doorway", nullptr}, {"baseboardHeight", 0.0}}), "trim") ==
          nullptr);
    CHECK(rv::geometry::findPart(buildOk("armchair", {{"legHeight", 0.0}}), "wood") == nullptr);
    // I6 step 3b: the sofa without legs or pillows, the desk without its computer, the empty showcase.
    CHECK(rv::geometry::findPart(buildOk("sofa", {{"legHeight", 0.0}}), "wood") == nullptr);
    CHECK(rv::geometry::findPart(buildOk("sofa", {{"pillows", false}}), "pillow") == nullptr);
    const ProceduralPrefab bareDesk = buildOk("desk", {{"computer", false}});
    CHECK(rv::geometry::findPart(bareDesk, "screen") == nullptr);
    CHECK(rv::geometry::findPart(bareDesk, "plastic_body") == nullptr);
    const ProceduralPrefab emptyCase = buildOk("showcase", {{"item", "none"}});
    CHECK(rv::geometry::findPart(emptyCase, "plastic_body") == nullptr);
    CHECK(rv::geometry::findPart(buildOk("showcase", {{"item", "handheld"}}), "plastic_accent") != nullptr);
    // The spine is optional: only with spineLabel, after the label.
    const ProceduralPrefab spined = buildOk("cartridge", {{"spineLabel", true}});
    REQUIRE(spined.parts.size() == 3);
    CHECK(spined.parts.back().materialRole == "spine");
    // The wainscot is optional, after the contract roles.
    const ProceduralPrefab paneled = buildOk("room_shell", {{"wainscotHeight", 1.0}});
    REQUIRE(paneled.parts.size() == 5);
    CHECK(paneled.parts.back().materialRole == "wainscot");
}

TEST_CASE("geometry/prefabs: bounds follow the documented dimensions and pivots", "[geometry][prefabs][bounds]") {
    struct Case {
        std::string_view kind;
        json params;
        Aabb bounds;
    };
    const auto box = [](glm::vec3 min, glm::vec3 max) {
        return Aabb::fromMinMax(min, max);
    };
    const std::vector<Case> cases{
        // Pivot floor, interior exactly "size", walls outside.
        {"room_shell", nullptr, box({-4.65f, 0.0f, -5.15f}, {4.65f, 3.0f, 5.15f})},
        {"room_shell",
         {{"size", {4.0, 6.0}}, {"wallHeight", 2.6}, {"wallThickness", 0.2}},
         box({-2.2f, 0.0f, -3.2f}, {2.2f, 2.6f, 3.2f})},
        // Pivot floor.
        {"shelf", nullptr, box({-0.6f, 0.0f, -0.175f}, {0.6f, 2.0f, 0.175f})},
        {"shelf", {{"width", 0.9}, {"height", 1.8}, {"depth", 0.3}}, box({-0.45f, 0.0f, -0.15f}, {0.45f, 1.8f, 0.15f})},
        // Pivot center.
        {"cartridge", nullptr, box({-0.0625f, -0.0425f, -0.01f}, {0.0625f, 0.0425f, 0.01f})},
        {"cartridge", {{"size", {0.14, 0.1, 0.022}}}, box({-0.07f, -0.05f, -0.011f}, {0.07f, 0.05f, 0.011f})},
        {"crt", nullptr, box({-0.355f, 0.0f, -0.28f}, {0.355f, 0.61f, 0.28f})},
        {"crt",
         {{"width", 0.5}, {"height", 0.45}, {"depth", 0.45}},
         box({-0.25f, 0.0f, -0.225f}, {0.25f, 0.45f, 0.225f})},
        {"tv_cabinet", nullptr, box({-0.6f, 0.0f, -0.25f}, {0.6f, 0.55f, 0.25f})},
        {"tv_cabinet",
         {{"width", 1.6}, {"height", 0.45}, {"depth", 0.4}, {"legHeight", 0.0}},
         box({-0.8f, 0.0f, -0.2f}, {0.8f, 0.45f, 0.2f})},
        // Legs too short to build: the carcass rests on the floor instead of floating.
        {"tv_cabinet", {{"legHeight", 5e-5}}, box({-0.6f, 0.0f, -0.25f}, {0.6f, 0.55f, 0.25f})},
        {"console_cartridge", nullptr, box({-0.12f, 0.0f, -0.1f}, {0.12f, 0.07f, 0.1f})},
        {"console_cartridge",
         {{"width", 0.3}, {"height", 0.08}, {"depth", 0.25}},
         box({-0.15f, 0.0f, -0.125f}, {0.15f, 0.08f, 0.125f})},
        // Pivot at the back center: z = 0 is the wall.
        {"poster_frame", nullptr, box({-0.25f, -0.35f, 0.0f}, {0.25f, 0.35f, 0.02f})},
        {"poster_frame",
         {{"width", 0.6}, {"height", 0.9}, {"depth", 0.03}},
         box({-0.3f, -0.45f, 0.0f}, {0.3f, 0.45f, 0.03f})},
        {"rug", nullptr, box({-1.2f, 0.0f, -0.8f}, {1.2f, 0.012f, 0.8f})},
        {"rug", {{"width", 3.0}, {"depth", 2.0}, {"thickness", 0.02}}, box({-1.5f, 0.0f, -1.0f}, {1.5f, 0.02f, 1.0f})},
        {"floor_lamp", nullptr, box({-0.22f, 0.0f, -0.22f}, {0.22f, 1.6f, 0.22f})},
        {"floor_lamp",
         {{"height", 1.8}, {"baseRadius", 0.25}, {"shadeRadius", 0.2}},
         box({-0.25f, 0.0f, -0.25f}, {0.25f, 1.8f, 0.25f})},
        {"armchair", nullptr, box({-0.41f, 0.0f, -0.41f}, {0.41f, 0.84f, 0.41f})},
        {"armchair",
         {{"width", 1.6}, {"depth", 0.9}, {"height", 0.95}, {"legHeight", 0.0}},
         box({-0.8f, 0.0f, -0.45f}, {0.8f, 0.95f, 0.45f})},
        // The spine label takes the shell's top half millimetre: the bounds stay the size.
        {"cartridge", {{"spineLabel", true}}, box({-0.0625f, -0.0425f, -0.01f}, {0.0625f, 0.0425f, 0.01f})},
        {"display_case", nullptr, box({-0.6f, 0.0f, -0.25f}, {0.6f, 0.95f, 0.25f})},
        {"display_case",
         {{"width", 0.8}, {"height", 1.05}, {"depth", 0.4}, {"baseHeight", 0.6}},
         box({-0.4f, 0.0f, -0.2f}, {0.4f, 1.05f, 0.2f})},
        // Hung on the wall: the floor pivot under it, the bottom at mountHeight.
        {"wall_shelf", nullptr, box({-0.45f, 1.1f, -0.04f}, {0.45f, 1.46f, 0.04f})},
        {"wall_shelf",
         {{"width", 0.6}, {"height", 0.5}, {"depth", 0.1}, {"mountHeight", 0.9}, {"ledges", 3}},
         box({-0.3f, 0.9f, -0.05f}, {0.3f, 1.4f, 0.05f})},
        // I6 step 3b's era furniture, pivot floor.
        {"floor_lamp", {{"shade", "torchiere"}}, box({-0.22f, 0.0f, -0.22f}, {0.22f, 1.6f, 0.22f})},
        {"sofa", nullptr, box({-0.8f, 0.0f, -0.43f}, {0.8f, 0.8f, 0.43f})},
        {"sofa",
         {{"width", 2.2}, {"depth", 0.95}, {"seats", 3}, {"legHeight", 0.0}},
         box({-1.1f, 0.0f, -0.475f}, {1.1f, 0.8f, 0.475f})},
        {"beanbag", nullptr, box({-0.425f, 0.0f, -0.425f}, {0.425f, 0.72f, 0.425f})},
        {"beanbag", {{"width", 1.0}, {"depth", 0.7}, {"height", 0.5}}, box({-0.5f, 0.0f, -0.35f}, {0.5f, 0.5f, 0.35f})},
        {"desk", nullptr, box({-0.65f, 0.0f, -0.35f}, {0.65f, 1.18f, 0.35f})},
        {"desk", {{"computer", false}, {"height", 0.72}}, box({-0.65f, 0.0f, -0.35f}, {0.65f, 0.72f, 0.35f})},
        {"office_chair", nullptr, box({-0.3f, 0.0f, -0.3f}, {0.3f, 0.95f, 0.3f})},
        {"office_chair", {{"arms", false}, {"width", 0.5}}, box({-0.25f, 0.0f, -0.3f}, {0.25f, 0.95f, 0.3f})},
        {"bar_stool", nullptr, box({-0.22f, 0.0f, -0.22f}, {0.22f, 0.76f, 0.22f})},
        {"bar_stool", {{"seatRadius", 0.25}}, box({-0.25f, 0.0f, -0.25f}, {0.25f, 0.76f, 0.25f})},
        {"banker_lamp", nullptr, box({-0.21f, 0.0f, -0.18f}, {0.21f, 1.475f, 0.18f})},
        {"showcase", nullptr, box({-0.4f, 0.0f, -0.18f}, {0.4f, 1.15f, 0.18f})},
        {"showcase",
         {{"width", 1.0}, {"height", 0.95}, {"depth", 0.45}, {"baseHeight", 0.5}, {"item", "handheld"}},
         box({-0.5f, 0.0f, -0.225f}, {0.5f, 0.95f, 0.225f})},
        // I6 step 5's hub, pivot floor: the column's plinth, the railing along X, the totem's plinth.
        {"column", nullptr, box({-0.28f, 0.0f, -0.28f}, {0.28f, 5.0f, 0.28f})},
        {"column", {{"radius", 0.15}, {"height", 3.0}, {"plinthSize", 0.4}}, box({-0.2f, 0.0f, -0.2f}, {0.2f, 3.0f, 0.2f})},
        {"railing", nullptr, box({-1.5f, 0.0f, -0.045f}, {1.5f, 0.95f, 0.045f})},
        {"railing", {{"length", 7.2}, {"height", 1.0}}, box({-3.6f, 0.0f, -0.045f}, {3.6f, 1.0f, 0.045f})},
        {"info_totem", nullptr, box({-0.3f, 0.0f, -0.16f}, {0.3f, 1.75f, 0.16f})},
        // I6 step 6: a pair of cabinets 4 cm apart, the moulding 5 mm proud of the cap's front; the hero cabinet alone.
        {"arcade_cabinet", nullptr, box({-0.44f, 0.0f, -0.2f}, {0.44f, 1.7f, 0.2f})},
        {"arcade_cabinet",
         {{"count", 1}, {"width", 0.7}, {"height", 1.85}, {"depth", 0.85}, {"slots", false}},
         box({-0.35f, 0.0f, -0.425f}, {0.35f, 1.85f, 0.425f})},
        // I5 step 3: the sideboard's footprint, the CRT on its top (0.5 + 0.4 m).
        {"secondary_tv", nullptr, box({-0.4f, 0.0f, -0.18f}, {0.4f, 0.9f, 0.18f})},
        {"secondary_tv",
         {{"width", 1.0}, {"height", 0.6}, {"depth", 0.45}, {"tvSize", {0.5, 0.45, 0.4}}, {"tvOffset", 0.25}},
         box({-0.5f, 0.0f, -0.225f}, {0.5f, 1.05f, 0.225f})},
        // A wainscot lies inside the walls' interior: the bounds stay the shell's.
        {"room_shell", {{"wainscotHeight", 1.0}}, box({-4.65f, 0.0f, -5.15f}, {4.65f, 3.0f, 5.15f})},
    };
    for (const Case& c : cases) {
        const ProceduralPrefab prefab = buildOk(c.kind, c.params);
        INFO(std::string(c.kind) + " " + c.params.dump());
        CHECK(closeTo(prefab.bounds, c.bounds));
    }
}

TEST_CASE("geometry/prefabs: sockets use the contract ids and lie on the prefab", "[geometry][prefabs][sockets]") {
    const std::map<std::string_view, std::vector<std::pair<std::string, SocketKind>>> expected{
        {"room_shell", {{"Doorway", SocketKind::Anchor}}},
        {"cartridge", {{"Label", SocketKind::Other}}},
        {"crt",
         {{"ScreenSurface", SocketKind::ScreenSurface},
          {"TV_Stand", SocketKind::Anchor},
          {"ScreenLook", SocketKind::Anchor},
          {"PowerButton", SocketKind::Other}}},
        {"tv_cabinet", {{"Top", SocketKind::Anchor}, {"ConsoleShelf", SocketKind::Anchor}}},
        {"console_cartridge", {{"CartridgeSlot", SocketKind::Anchor}, {"Front", SocketKind::Other}}},
        {"poster_frame", {{"Picture", SocketKind::Other}}},
        {"rug", {}},
        {"floor_lamp", {{"LightSource", SocketKind::Anchor}}},
        {"armchair", {{"Seat", SocketKind::Anchor}}},
        {"storage_box", {}},
        {"magazine_stack", {}},
        {"bookends", {}},
        {"plant", {}},
        {"sofa", {{"Seat", SocketKind::Anchor}}},
        {"beanbag", {}},
        {"desk", {{"Top", SocketKind::Anchor}, {"Screen", SocketKind::Other}}},
        {"office_chair", {{"Seat", SocketKind::Anchor}}},
        {"bar_stool", {{"Seat", SocketKind::Anchor}}},
        {"banker_lamp", {{"LightSource", SocketKind::Anchor}}},
        {"showcase", {{"Item", SocketKind::Anchor}}},
        {"column", {}},
        {"railing", {}},
        {"hub_landmark", {{"Globe", SocketKind::Anchor}}},
        {"info_totem", {{"Screen", SocketKind::Anchor}}},
        {"arcade_cabinet",
         {{"Level_0", SocketKind::Anchor}, {"Slot_0_0", SocketKind::ShelfSlot}, {"Slot_0_1", SocketKind::ShelfSlot}}},
        {"secondary_tv",
         {{"ScreenSurface", SocketKind::ScreenSurface},
          {"TV_Stand", SocketKind::Anchor},
          {"ScreenLook", SocketKind::Anchor},
          {"PowerButton", SocketKind::Other},
          {"ConsoleShelf", SocketKind::Anchor}}},
    };
    for (const auto& [kind, sockets] : expected) {
        const ProceduralPrefab prefab = buildOk(kind);
        INFO(kind);
        CHECK(prefab.sockets.size() == sockets.size());
        for (const auto& [id, socketKind] : sockets) {
            CHECK(socket(prefab, id).kind == socketKind);
        }
    }
    for (const std::string_view kind : rv::geometry::prefabKinds()) {
        const ProceduralPrefab prefab = buildOk(kind);
        for (const Socket& s : prefab.sockets) {
            if (s.id == "ScreenLook") {
                continue; // a viewpoint in front of the screen, outside the set by definition
            }
            INFO(std::string(kind) + " " + s.id);
            CHECK(prefab.bounds.inflated(1e-5f).contains(position(s)));
            if (!s.bounds.isEmpty()) {
                CHECK(prefab.bounds.inflated(1e-4f).contains(s.bounds.transformed(s.transform)));
            }
        }
    }
    // A shelf has slots and, per level, the Level anchor of its free volume.
    const ProceduralPrefab shelf = buildOk("shelf");
    for (const Socket& s : shelf.sockets) {
        INFO(s.id);
        if (rv::geometry::parseLevelId(s.id)) {
            CHECK(s.kind == SocketKind::Anchor);
        } else {
            CHECK(s.kind == SocketKind::ShelfSlot);
            CHECK(rv::geometry::parseSlotId(s.id).has_value());
        }
    }
}

TEST_CASE("geometry/socket: the size of a socket follows the scale of its transform", "[geometry][sockets]") {
    Socket screen;
    screen.bounds = Aabb::fromMinMax(glm::vec3(-0.24f, -0.18f, 0.0f), glm::vec3(0.24f, 0.18f, 0.0f));
    CHECK(rv::geometry::socketSize(screen) == glm::vec3(0.48f, 0.36f, 0.0f));
    // A node exported in centimeters (scale 0.01), turned a quarter around Y: each side scales with its own axis.
    screen.transform = glm::rotate(glm::scale(glm::mat4(1.0f), glm::vec3(0.01f, 0.02f, 0.01f)), glm::radians(90.0f),
                                   glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 scaled = rv::geometry::socketSize(screen);
    CHECK(scaled.x == Approx(0.0048f));
    CHECK(scaled.y == Approx(0.0072f));
    CHECK(scaled.z == Approx(0.0f).margin(1e-7f));
    // Nothing to measure: an anchor, or bounds that are not finite.
    CHECK(rv::geometry::socketSize(Socket{}) == glm::vec3(0.0f));
    screen.bounds = Aabb::fromMinMax(glm::vec3(0.0f), glm::vec3(std::numeric_limits<float>::infinity()));
    CHECK(rv::geometry::socketSize(screen) == glm::vec3(0.0f));
}

TEST_CASE("geometry/prefabs: shelf slots follow the layout formula", "[geometry][prefabs][shelf]") {
    struct Layout {
        json params;
        double width, height, board, plinth, margin, gap;
        int levels;
        glm::dvec3 item;
        bool back;
        double depth;
    };
    const std::vector<Layout> layouts{
        {nullptr, 1.2, 2.0, 0.025, 0.08, 0.03, 0.004, 5, {0.125, 0.085, 0.020}, true, 0.35},
        {{{"width", 0.8},
          {"height", 1.2},
          {"depth", 0.3},
          {"levels", 3},
          {"boardThickness", 0.02},
          {"back", false},
          {"plinthHeight", 0.0},
          {"slotSize", {0.135, 0.19, 0.014}},
          {"slotGap", 0.002},
          {"slotMargin", 0.02}},
         0.8,
         1.2,
         0.02,
         0.0,
         0.02,
         0.002,
         3,
         {0.135, 0.19, 0.014},
         false,
         0.3},
        // The slots fill the inner width exactly: (0.45 - 0.05 + 0.004) / 0.101 = 4.
        {{{"width", 0.5}, {"slotSize", {0.097, 0.085, 0.02}}, {"slotGap", 0.004}, {"slotMargin", 0.025}},
         0.5,
         2.0,
         0.025,
         0.08,
         0.025,
         0.004,
         5,
         {0.097, 0.085, 0.02},
         true,
         0.35},
    };
    for (const Layout& l : layouts) {
        INFO(l.params.dump());
        const ProceduralPrefab shelf = buildOk("shelf", l.params);
        const double innerWidth = l.width - 2.0 * l.board;
        const int perLevel =
            static_cast<int>(std::floor((innerWidth - 2.0 * l.margin + l.gap) / (l.item.x + l.gap) + 1e-9));
        const double pitch = (l.height - l.board - l.plinth) / l.levels;
        REQUIRE(perLevel >= 1);
        CHECK(shelf.sockets.size() == static_cast<std::size_t>(l.levels * (perLevel + 1))); // the slots and Level_<n>
        CHECK(rv::geometry::findSocket(shelf.sockets, "Slot_" + std::to_string(l.levels) + "_0") == nullptr);
        CHECK(rv::geometry::findSocket(shelf.sockets, "Slot_0_" + std::to_string(perLevel)) == nullptr);
        // The boards run from the 8 mm back panel (the back edge without one) to the front edge.
        const double boardBack = -0.5 * l.depth + (l.back ? 0.008 : 0.0);
        const double boardFront = 0.5 * l.depth;
        if (l.back) {
            CHECK(part(shelf, "wood_secondary").mesh.bounds.max.z == Approx(boardBack).margin(1e-6));
        }

        const glm::vec3 item(l.item);
        const Aabb itemBounds =
            Aabb::fromMinMax({-0.5f * item.x, 0.0f, -0.5f * item.z}, {0.5f * item.x, item.y, 0.5f * item.z});
        std::vector<Aabb> volumes;
        for (int level = 0; level < l.levels; ++level) {
            // The level's anchor: on the board, the compartment's free volume, holding every slot of the level.
            const Socket& anchor = socket(shelf, "Level_" + std::to_string(level));
            CHECK(anchor.kind == SocketKind::Anchor);
            CHECK(anchor.nodeName == "Anchor_" + anchor.id);
            CHECK(closeTo(position(anchor), glm::vec3(0.0f, static_cast<float>(l.plinth + level * pitch + l.board),
                                                      static_cast<float>(0.5 * (boardBack + boardFront))),
                          1e-5f));
            CHECK(closeTo(anchor.bounds.size(),
                          glm::vec3(static_cast<float>(innerWidth), static_cast<float>(pitch - l.board),
                                    static_cast<float>(boardFront - boardBack)),
                          1e-5f));
            const Aabb free = anchor.bounds.transformed(anchor.transform).inflated(1e-5f);
            for (int index = 0; index < perLevel; ++index) {
                const Socket& s = socket(shelf, "Slot_" + std::to_string(level) + "_" + std::to_string(index));
                CHECK(free.contains(s.bounds.transformed(s.transform)));
            }
            for (int index = 0; index < perLevel; ++index) {
                const Socket& s = socket(shelf, "Slot_" + std::to_string(level) + "_" + std::to_string(index));
                CHECK(s.nodeName == s.id);
                CHECK(s.kind == SocketKind::ShelfSlot);
                CHECK(closeTo(s.bounds, itemBounds));
                const glm::vec3 p = position(s);
                // Left-aligned from the inner side, bottom on the board of its level.
                CHECK(p.x ==
                      Approx(-innerWidth / 2 + l.margin + index * (l.item.x + l.gap) + l.item.x / 2).margin(1e-5));
                CHECK(p.y == Approx(l.plinth + level * pitch + l.board).margin(1e-5));
                // Centered in the board depth.
                CHECK(p.z == Approx(0.5 * (boardBack + boardFront)).margin(1e-6));
                const Aabb volume = s.bounds.transformed(s.transform);
                // Inside the interior: between the side panels, under the next board, on the board in front of
                // the back panel.
                CHECK(volume.min.x >= -innerWidth / 2 - 1e-5);
                CHECK(volume.max.x <= innerWidth / 2 + 1e-5);
                CHECK(volume.max.y <= l.plinth + (level + 1) * pitch + 1e-5);
                CHECK(volume.max.z <= boardFront + 1e-6);
                CHECK(volume.min.z >= boardBack - 1e-6);
                volumes.push_back(volume.inflated(-1e-5f));
            }
        }
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            for (std::size_t j = i + 1; j < volumes.size(); ++j) {
                CHECK_FALSE(volumes[i].intersects(volumes[j]));
            }
        }
    }
}

TEST_CASE("geometry/prefabs: crt screen keeps its aspect and faces +Z", "[geometry][prefabs][crt]") {
    for (const double aspect : {1.3333333, 1.7777778, 1.25, 1.0, 2.2}) {
        INFO(aspect);
        const ProceduralPrefab crt = buildOk("crt", {{"screenAspect", aspect}});
        const MeshData& screen = part(crt, "screen").mesh;
        const glm::vec3 size = screen.bounds.size();
        CHECK(size.x / size.y == Approx(aspect).epsilon(1e-4));
        // Inside the bezel (6 cm) and behind the bezel front.
        CHECK(screen.bounds.min.x >= -0.355f + 0.06f - 1e-5f);
        CHECK(screen.bounds.max.x <= 0.355f - 0.06f + 1e-5f);
        CHECK(screen.bounds.max.y <= 0.61f - 0.06f + 1e-5f);
        CHECK(screen.bounds.max.z < 0.28f);

        const Socket& surface = socket(crt, "ScreenSurface");
        CHECK(closeTo(axis(surface, 2), glm::vec3(0.0f, 0.0f, 1.0f)));
        CHECK(closeTo(axis(surface, 0), glm::vec3(1.0f, 0.0f, 0.0f)));
        CHECK(position(surface).x == Approx(0.0f).margin(1e-6));
        CHECK(position(surface).y == Approx(screen.bounds.center().y));
        CHECK(position(surface).z == Approx(screen.bounds.min.z));
        CHECK(surface.bounds.size().x == Approx(size.x));
        CHECK(surface.bounds.size().y == Approx(size.y));
        CHECK(surface.bounds.size().x / surface.bounds.size().y == Approx(aspect).epsilon(1e-4));
        // Mesh-backed like a glTF ScreenSurface node: the socket names the screen part.
        REQUIRE(surface.meshIndex.has_value());
        REQUIRE(*surface.meshIndex < crt.parts.size());
        CHECK(crt.parts[*surface.meshIndex].materialRole == "screen");

        const Socket& look = socket(crt, "ScreenLook");
        CHECK(closeTo(position(look), glm::vec3(0.0f, position(surface).y, 0.28f + 0.9f)));
        CHECK(look.transform[0] == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        CHECK(look.transform[2] == glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)); // looks along its -Z, at the screen
    }

    const ProceduralPrefab crt = buildOk("crt");
    const MeshData& screen = part(crt, "screen").mesh;
    // uv (0, 0) at the top-left corner of the screen, (1, 1) at the bottom-right corner.
    for (const rv::geometry::Vertex& v : screen.vertices) {
        if (v.uv0 == glm::vec2(0.0f, 0.0f)) {
            CHECK(v.position.x == Approx(screen.bounds.min.x));
            CHECK(v.position.y == Approx(screen.bounds.max.y));
        }
        if (v.uv0 == glm::vec2(1.0f, 1.0f)) {
            CHECK(v.position.x == Approx(screen.bounds.max.x));
            CHECK(v.position.y == Approx(screen.bounds.min.y));
        }
        CHECK(v.normal.z > 0.9f);
    }
    CHECK(socket(crt, "TV_Stand").transform == glm::mat4(1.0f));
    const glm::vec3 power = position(socket(crt, "PowerButton"));
    CHECK(power.x > 0.0f);
    CHECK(power.y < screen.bounds.min.y);
    CHECK(power.z > 0.25f);
    CHECK(power.z <= 0.28f);
}

TEST_CASE("geometry/prefabs: crt builds flat and nearly flat backs at any size", "[geometry][prefabs][crt]") {
    // A rear section kept at full size (backTaper 0, or so small that 1 - taper rounds to 1) used to clamp its
    // center between bounds that float rounding inverts for many sizes, these three among them: undefined
    // behavior, an abort in debug builds.
    for (const double taper : {0.0, 1e-7}) {
        for (const glm::dvec2 size : {glm::dvec2(0.5, 0.45), glm::dvec2(0.45, 0.4), glm::dvec2(0.55, 0.48)}) {
            const json params{{"width", size.x}, {"height", size.y}, {"backTaper", taper}};
            INFO(params.dump());
            const ProceduralPrefab crt = buildOk("crt", params);
            checkPrefab(crt);
            CHECK(closeTo(crt.bounds.size(), glm::vec3(glm::vec2(size), 0.56f)));
        }
    }
    for (int centimeters = 30; centimeters <= 60; ++centimeters) {
        const double height = centimeters / 100.0;
        const json params{{"width", 0.5}, {"height", height}, {"backTaper", 0.0}};
        INFO(params.dump());
        CHECK(closeTo(buildOk("crt", params).bounds.size(), glm::vec3(0.5f, static_cast<float>(height), 0.56f)));
    }
}

TEST_CASE("geometry/prefabs: room shell doorway stays open and interior faces point inward",
          "[geometry][prefabs][room_shell]") {
    const ProceduralPrefab room = buildOk("room_shell");
    const Socket& doorway = socket(room, "Doorway");
    CHECK(closeTo(position(doorway), glm::vec3(0.0f, 0.0f, 5.075f)));
    CHECK(closeTo(axis(doorway, 2), glm::vec3(0.0f, 0.0f, 1.0f)));
    CHECK(closeTo(doorway.bounds, Aabb::fromMinMax({-0.8f, 0.0f, -0.075f}, {0.8f, 2.3f, 0.075f})));

    // Rays from the room center through the door rectangle reach the outside without touching anything.
    const glm::vec3 out(0.0f, 0.0f, 1.0f);
    for (const glm::vec2 xy : {glm::vec2(0.0f, 1.15f), glm::vec2(0.79f, 0.01f), glm::vec2(-0.79f, 2.29f)}) {
        INFO(xy.x << ", " << xy.y);
        CHECK(rayHits(room, rv::core::Ray{glm::vec3(xy, 0.0f), out}, 20.0f) == 0);
    }
    // Beside and above the door, the same rays hit the wall, its lining or its casing.
    CHECK(rayHits(room, rv::core::Ray{glm::vec3(1.2f, 1.15f, 0.0f), out}, 20.0f) > 0);
    CHECK(rayHits(room, rv::core::Ray{glm::vec3(0.0f, 2.6f, 0.0f), out}, 20.0f) > 0);

    const ProceduralPrefab closed = buildOk("room_shell", {{"doorway", nullptr}});
    CHECK(closed.sockets.empty());
    CHECK(rayHits(closed, rv::core::Ray{glm::vec3(0.0f, 1.15f, 0.0f), out}, 20.0f) > 0);

    // Doorways on the other walls; the offset runs along +X (north, south) or +Z (east, west).
    struct Door {
        std::string wall;
        float offset;
        glm::vec3 socketPosition;
        glm::vec3 outward;
    };
    for (const Door& d : {Door{"north", 1.0f, {1.0f, 0.0f, -5.075f}, {0.0f, 0.0f, -1.0f}},
                          Door{"east", -2.0f, {4.575f, 0.0f, -2.0f}, {1.0f, 0.0f, 0.0f}},
                          Door{"west", 1.5f, {-4.575f, 0.0f, 1.5f}, {-1.0f, 0.0f, 0.0f}}}) {
        INFO(d.wall);
        const ProceduralPrefab r = buildOk("room_shell", {{"doorway", {{"wall", d.wall}, {"offset", d.offset}}}});
        checkPrefab(r);
        const Socket& s = socket(r, "Doorway");
        CHECK(closeTo(position(s), d.socketPosition));
        CHECK(closeTo(axis(s, 2), d.outward));
        CHECK(closeTo(axis(s, 1), glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 inside = d.socketPosition - 4.0f * d.outward + glm::vec3(0.0f, 1.15f, 0.0f);
        CHECK(rayHits(r, rv::core::Ray{inside, d.outward}, 20.0f) == 0);
        // The mirrored position on the same wall is solid.
        const glm::vec3 along =
            glm::abs(d.outward.x) > 0.5f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 mirrored = inside - 2.0f * glm::dot(inside, along) * along;
        CHECK(rayHits(r, rv::core::Ray{mirrored, d.outward}, 20.0f) > 0);
    }

    // Interior faces of the walls point into the room; floor up, ceiling down.
    const MeshData& walls = part(room, "wall").mesh;
    std::map<std::string, int> planes;
    for (std::size_t t = 0; t < walls.triangleCount(); ++t) {
        const rv::test::TriangleRef tri = rv::test::triangleAt(walls, t);
        const glm::vec3 n = glm::normalize(tri.cross());
        const auto onPlane = [&](int axisIndex, float value) {
            return std::fabs(tri.v0.position[axisIndex] - value) < 1e-5f &&
                   std::fabs(tri.v1.position[axisIndex] - value) < 1e-5f &&
                   std::fabs(tri.v2.position[axisIndex] - value) < 1e-5f;
        };
        if (onPlane(2, 5.0f)) {
            CHECK(n.z == Approx(-1.0f));
            ++planes["south"];
        } else if (onPlane(2, -5.0f)) {
            CHECK(n.z == Approx(1.0f));
            ++planes["north"];
        } else if (onPlane(0, 4.5f)) {
            CHECK(n.x == Approx(-1.0f));
            ++planes["east"];
        } else if (onPlane(0, -4.5f)) {
            CHECK(n.x == Approx(1.0f));
            ++planes["west"];
        }
    }
    CHECK(planes.size() == 4);
    for (const rv::geometry::Vertex& v : part(room, "floor").mesh.vertices) {
        CHECK(v.normal == glm::vec3(0.0f, 1.0f, 0.0f));
        CHECK(v.position.y == 0.0f);
    }
    for (const rv::geometry::Vertex& v : part(room, "ceiling").mesh.vertices) {
        CHECK(v.normal == glm::vec3(0.0f, -1.0f, 0.0f));
        CHECK(v.position.y == 3.0f);
    }

    // Floor and wall uvs are in meters / metersPerTile.
    const ProceduralPrefab tiled = buildOk("room_shell", {{"metersPerTile", 2.0}});
    for (const rv::geometry::Vertex& v : part(tiled, "floor").mesh.vertices) {
        CHECK(v.uv0.x == Approx(v.position.x / 2.0f).margin(1e-5));
        CHECK(v.uv0.y == Approx(v.position.z / 2.0f).margin(1e-5));
    }
    std::size_t northInterior = 0;
    for (const rv::geometry::Vertex& v : part(tiled, "wall").mesh.vertices) {
        if (closeTo(v.normal, glm::vec3(0.0f, 0.0f, 1.0f)) && std::fabs(v.position.z + 5.0f) < 1e-5f) {
            ++northInterior;
            CHECK(v.uv0.x == Approx(v.position.x / 2.0f).margin(1e-5));
            CHECK(v.uv0.y == Approx(-v.position.y / 2.0f).margin(1e-5));
        }
    }
    CHECK(northInterior == 4);
}

TEST_CASE("geometry/prefabs: wooden parts have uvs in meters along their boards", "[geometry][prefabs][uv]") {
    // One uv unit per meter (BoxUv::GrainMeters): a wood texture keeps one scale on every board, its grain along u
    // following the board's length: the u extent of each flat face is its longer side, in meters.
    const auto checkGrain = [](const PrefabPart& wood) {
        INFO(wood.mesh.name);
        std::size_t faces = 0;
        for (std::size_t t = 0; t < wood.mesh.triangleCount(); ++t) {
            const rv::test::TriangleRef tri = rv::test::triangleAt(wood.mesh, t);
            const glm::vec3 e1 = tri.v1.position - tri.v0.position;
            const glm::vec3 e2 = tri.v2.position - tri.v0.position;
            const glm::vec2 d1 = tri.v1.uv0 - tri.v0.uv0;
            const glm::vec2 d2 = tri.v2.uv0 - tri.v0.uv0;
            // Meters per uv unit: the triangle's area over its uv area.
            const glm::vec3 cross = glm::cross(e1, e2);
            const float area = 0.5f * glm::length(cross);
            const float uvArea = 0.5f * std::fabs(d1.x * d2.y - d2.x * d1.y);
            // Flat faces only: the 45-degree bevels of chamfered edges reuse the uvs of the face they lean towards.
            const glm::vec3 n = glm::abs(cross) / std::max(2.0f * area, 1e-12f);
            if (area < 1e-6f || uvArea < 1e-12f || std::max({n.x, n.y, n.z}) < 0.999f) {
                continue;
            }
            CHECK(std::sqrt(area / uvArea) == Approx(1.0f).epsilon(1e-3));
            ++faces;
        }
        CHECK(faces > 0);
    };
    checkGrain(part(buildOk("shelf"), "wood_primary"));
    checkGrain(part(buildOk("shelf"), "wood_secondary"));
    checkGrain(part(buildOk("tv_cabinet"), "wood_primary"));
    checkGrain(part(buildOk("tv_cabinet"), "wood_secondary"));
    checkGrain(part(buildOk("poster_frame"), "frame"));
    checkGrain(part(buildOk("room_shell"), "trim"));
    checkGrain(part(buildOk("room_shell", {{"wainscotHeight", 1.0}}), "wainscot"));

    // The tall side panel of a shelf: u runs up the board (its longer side), v across it.
    const ProceduralPrefab shelf = buildOk("shelf");
    float uMin = 1e9f;
    float uMax = -1e9f;
    for (const rv::geometry::Vertex& v : part(shelf, "wood_primary").mesh.vertices) {
        if (closeTo(v.normal, glm::vec3(-1.0f, 0.0f, 0.0f)) && v.position.x < -0.59f) {
            uMin = std::min(uMin, v.uv0.x);
            uMax = std::max(uMax, v.uv0.x);
        }
    }
    CHECK(uMax - uMin == Approx(2.0f).margin(0.01f));
}

TEST_CASE("geometry/prefabs: horizontal boards bevel their top edges only", "[geometry][prefabs][shelf]") {
    // A bevel along the bottom edge of a board faces away from the ceiling lights: from a few meters away it is a dark
    // strip thinner than a pixel that aliases into dashes under the board's bright front edge. The shelf's boards and
    // the cabinet top keep their top bevels (the front one catches the light) and square bottom edges.
    const auto downwardBevels = [](const PrefabPart& wood, float minY) {
        std::size_t count = 0;
        for (const rv::geometry::Vertex& v : wood.mesh.vertices) {
            count += (v.position.y > minY && v.normal.y < -0.1f && v.normal.y > -0.99f) ? 1u : 0u;
        }
        return count;
    };
    const auto upwardBevels = [](const PrefabPart& wood, float minY) {
        std::size_t count = 0;
        for (const rv::geometry::Vertex& v : wood.mesh.vertices) {
            count += (v.position.y > minY && v.normal.y > 0.1f && v.normal.y < 0.99f) ? 1u : 0u;
        }
        return count;
    };
    // Above the floor, where only the side panels' bottom edges are bevelled.
    const ProceduralPrefab shelf = buildOk("shelf");
    CHECK(downwardBevels(part(shelf, "wood_primary"), 0.01f) == 0);
    CHECK(upwardBevels(part(shelf, "wood_primary"), 0.01f) > 0);
    // The cabinet top: 3 cm thick, at the top of the default 0.55 m cabinet.
    const ProceduralPrefab cabinet = buildOk("tv_cabinet");
    CHECK(downwardBevels(part(cabinet, "wood_primary"), 0.55f - 0.03f - 1e-4f) == 0);
    CHECK(upwardBevels(part(cabinet, "wood_primary"), 0.55f - 0.03f - 1e-4f) > 0);
}

TEST_CASE("geometry/prefabs: a shelf lit inside: a strip under each board, a glow on each compartment's back",
          "[geometry][prefabs][shelf]") {
    // I6 step 3 (look.md: lit shelf interiors): ShelfParams::boardLight adds two parts after the contract roles and
    // changes nothing else; by default the shelf is as before.
    const ProceduralPrefab plain = buildOk("shelf");
    CHECK(rv::geometry::findPart(plain, "board_light") == nullptr);
    CHECK(rv::geometry::findPart(plain, "board_glow") == nullptr);
    const ProceduralPrefab lit = buildOk("shelf", {{"boardLight", true}});
    REQUIRE(lit.parts.size() == 4);
    CHECK(lit.parts[2].materialRole == "board_light");
    CHECK(lit.parts[3].materialRole == "board_glow");
    CHECK(lit.bounds.min == plain.bounds.min);
    CHECK(lit.bounds.max == plain.bounds.max);
    REQUIRE(lit.sockets.size() == plain.sockets.size());
    for (std::size_t i = 0; i < plain.sockets.size(); ++i) {
        CHECK(lit.sockets[i].id == plain.sockets[i].id);
        CHECK(lit.sockets[i].transform == plain.sockets[i].transform);
    }
    CHECK(part(lit, "wood_primary").mesh.vertices.size() == part(plain, "wood_primary").mesh.vertices.size());
    // The default shelf: 5 levels of 2 m, 2.5 cm boards over an 8 cm plinth, 35 cm deep with an 8 mm back.
    const rv::geometry::ShelfParams p;
    const float pitch = (p.height - p.boardThickness - p.plinthHeight) / static_cast<float>(p.levels);
    // One strip (its underside and front, 8 vertices) under each of the five boards above a compartment, facing down
    // and out, just under the board, 2 cm behind the front edge.
    const PrefabPart& strips = part(lit, "board_light");
    REQUIRE(strips.mesh.vertices.size() == static_cast<std::size_t>(p.levels) * 8u);
    std::set<int> undersides;
    for (const rv::geometry::Vertex& v : strips.mesh.vertices) {
        CHECK((v.normal.y < -0.99f || v.normal.z > 0.99f));
        CHECK(v.position.z <= 0.5f * p.depth - 0.02f + 1e-5f);
        CHECK(v.position.z >= 0.5f * p.depth - 0.032f - 1e-5f);
        if (v.normal.y < -0.99f) {
            undersides.insert(static_cast<int>(std::lround((v.position.y + 0.003f) * 10000.0f)));
        }
    }
    std::set<int> boards;
    for (int level = 1; level <= p.levels; ++level) {
        const float underside =
            level < p.levels ? p.plinthHeight + static_cast<float>(level) * pitch : p.height - p.boardThickness;
        boards.insert(static_cast<int>(std::lround(underside * 10000.0f)));
    }
    CHECK(undersides == boards);
    // A glow quad over the back panel of each compartment, facing +Z 1.5 mm in front of it, its picture upright.
    const PrefabPart& glow = part(lit, "board_glow");
    REQUIRE(glow.mesh.vertices.size() == static_cast<std::size_t>(p.levels) * 4u);
    for (const rv::geometry::Vertex& v : glow.mesh.vertices) {
        CHECK(v.normal.z == Approx(1.0f));
        CHECK(v.position.z == Approx(-0.5f * p.depth + 0.008f + 0.0015f));
    }
    float topV = 1.0f;
    float topY = -1.0f;
    for (const rv::geometry::Vertex& v : glow.mesh.vertices) {
        if (v.position.y > topY) {
            topY = v.position.y;
            topV = v.uv0.y;
        }
    }
    CHECK(topY == Approx(p.height - p.boardThickness));
    CHECK(topV == Approx(0.0f)); // uv (0, 0) at the top-left: the glow's strongest edge under the strip
    // Without a back panel, no glow; the wall shelf gets strips under its cap and its upper ledges.
    CHECK(rv::geometry::findPart(buildOk("shelf", {{"boardLight", true}, {"back", false}}), "board_glow") == nullptr);
    const ProceduralPrefab wall = buildOk("wall_shelf", {{"boardLight", true}});
    const rv::geometry::WallShelfParams w;
    CHECK(part(wall, "board_light").mesh.vertices.size() == static_cast<std::size_t>(w.ledges) * 8u);
    CHECK(rv::geometry::findPart(buildOk("wall_shelf"), "board_light") == nullptr);
    CHECK(buildError("shelf", {{"boardLight", 1}}).toString().find("boardLight") != std::string::npos);
}

TEST_CASE("geometry/prefabs: the wainscot lines the walls and stops at the doorway", "[geometry][prefabs][wainscot]") {
    const ProceduralPrefab room = buildOk("room_shell", {{"wainscotHeight", 1.0}});
    const PrefabPart& wainscot = part(room, "wainscot");
    // On the walls' interior faces, from the baseboard to the rail top, never deeper than the rail.
    CHECK(wainscot.mesh.bounds.min.y == Approx(0.1f).margin(1e-5));
    CHECK(wainscot.mesh.bounds.max.y == Approx(1.0f).margin(1e-5));
    CHECK(closeTo(glm::vec3(wainscot.mesh.bounds.min.x, 0.0f, wainscot.mesh.bounds.min.z),
                  glm::vec3(-4.5f, 0.0f, -5.0f)));
    for (const rv::geometry::Vertex& v : wainscot.mesh.vertices) {
        const float fromWall = std::min({v.position.x + 4.5f, 4.5f - v.position.x, v.position.z + 5.0f,
                                         5.0f - v.position.z});
        CHECK(fromWall <= 0.022f + 1e-5f);
    }
    // The door stays clear: nothing of the wainscot crosses the opening (south wall, 1.6 m wide, with its casing).
    for (const rv::geometry::Vertex& v : wainscot.mesh.vertices) {
        if (v.position.z > 4.9f) {
            CHECK(std::fabs(v.position.x) >= 0.8f + 0.07f - 1e-5f);
        }
    }
    // Colliders are unchanged: the wainscot is thin trim the walker brushes past.
    CHECK(room.colliders.size() == buildOk("room_shell").colliders.size());
}

TEST_CASE("geometry/prefabs: furniture collides as its bounds, rooms as their walls",
          "[geometry][prefabs][colliders]") {
    for (const std::string_view kind : rv::geometry::prefabKinds()) {
        if (kind == "room_shell" || kind == "rug") {
            continue;
        }
        INFO(kind);
        const ProceduralPrefab prefab = buildOk(kind);
        REQUIRE(prefab.colliders.size() == 1);
        CHECK(prefab.colliders[0].name.empty());
        CHECK(prefab.colliders[0].box == prefab.bounds);
    }
    CHECK(buildOk("rug").colliders.empty()); // walked over

    const auto byName = [](const ProceduralPrefab& prefab) {
        std::map<std::string, Aabb> boxes;
        for (const rv::geometry::PrefabCollider& collider : prefab.colliders) {
            boxes.emplace(collider.name, collider.box);
        }
        return boxes;
    };
    // Defaults: interior 9 x 10 m, walls 0.15 m thick and 3 m tall, a 1.6 x 2.3 m doorway centered in the south wall.
    const ProceduralPrefab room = buildOk("room_shell");
    const std::map<std::string, Aabb> walls = byName(room);
    REQUIRE(walls.size() == 7);
    CHECK(closeTo(walls.at("north"), Aabb::fromMinMax({-4.65f, 0.0f, -5.15f}, {4.65f, 3.0f, -5.0f})));
    CHECK(closeTo(walls.at("east"), Aabb::fromMinMax({4.5f, 0.0f, -5.0f}, {4.65f, 3.0f, 5.0f})));
    CHECK(closeTo(walls.at("west"), Aabb::fromMinMax({-4.65f, 0.0f, -5.0f}, {-4.5f, 3.0f, 5.0f})));
    CHECK(closeTo(walls.at("south_west"), Aabb::fromMinMax({-4.65f, 0.0f, 5.0f}, {-0.8f, 3.0f, 5.15f})));
    CHECK(closeTo(walls.at("south_east"), Aabb::fromMinMax({0.8f, 0.0f, 5.0f}, {4.65f, 3.0f, 5.15f})));
    CHECK(closeTo(walls.at("south_above"), Aabb::fromMinMax({-0.8f, 2.3f, 5.0f}, {0.8f, 3.0f, 5.15f})));
    // Nothing lies beyond the opening: a 2 cm stop in the wall's outer face closes the door rectangle.
    CHECK(closeTo(walls.at("south_doorway"), Aabb::fromMinMax({-0.8f, 0.0f, 5.13f}, {0.8f, 2.3f, 5.15f})));

    // The interior and the door rectangle up to the stop stay free (touching their faces only).
    const Aabb interior = Aabb::fromMinMax({-4.5f, 0.0f, -5.0f}, {4.5f, 3.0f, 5.0f}).inflated(-1e-3f);
    const Aabb door = Aabb::fromMinMax({-0.8f, 0.0f, 4.9f}, {0.8f, 2.3f, 5.13f}).inflated(-1e-3f);
    for (const rv::geometry::PrefabCollider& collider : room.colliders) {
        INFO(collider.name);
        CHECK_FALSE(collider.box.intersects(interior));
        CHECK_FALSE(collider.box.intersects(door));
    }
    // The ring is closed: points inside every wall slab, corners and the doorway's outer face included, are covered.
    const auto covered = [&](const glm::vec3& point) {
        return std::any_of(room.colliders.begin(), room.colliders.end(),
                           [&](const rv::geometry::PrefabCollider& collider) { return collider.box.contains(point); });
    };
    for (const glm::vec3 point :
         {glm::vec3(0.0f, 1.0f, -5.07f), glm::vec3(4.57f, 1.0f, 0.0f), glm::vec3(-4.57f, 1.0f, 0.0f),
          glm::vec3(2.0f, 1.0f, 5.07f), glm::vec3(-2.0f, 1.0f, 5.07f), glm::vec3(0.0f, 2.6f, 5.07f),
          glm::vec3(4.6f, 1.0f, 5.1f), glm::vec3(-4.6f, 1.0f, -5.1f), glm::vec3(0.0f, 1.0f, 5.14f),
          glm::vec3(0.79f, 0.1f, 5.14f)}) {
        INFO(point.x << ", " << point.y << ", " << point.z);
        CHECK(covered(point));
    }
    CHECK_FALSE(covered(glm::vec3(0.0f, 1.0f, 5.07f))); // in the doorway

    // A closed room has the four full walls; a doorway elsewhere splits that wall, named by direction.
    const ProceduralPrefab closed = buildOk("room_shell", {{"doorway", nullptr}});
    std::set<std::string> closedNames;
    for (const auto& [name, box] : byName(closed)) {
        closedNames.insert(name);
    }
    CHECK(closedNames == std::set<std::string>{"east", "north", "south", "west"});
    const std::map<std::string, Aabb> east =
        byName(buildOk("room_shell", {{"doorway", {{"wall", "east"}, {"offset", -2.0}}}}));
    REQUIRE(east.size() == 7);
    CHECK(closeTo(east.at("east_north"), Aabb::fromMinMax({4.5f, 0.0f, -5.0f}, {4.65f, 3.0f, -2.8f})));
    CHECK(closeTo(east.at("east_south"), Aabb::fromMinMax({4.5f, 0.0f, -1.2f}, {4.65f, 3.0f, 5.0f})));
    CHECK(closeTo(east.at("east_above"), Aabb::fromMinMax({4.5f, 2.3f, -2.8f}, {4.65f, 3.0f, -1.2f})));
    CHECK(closeTo(east.at("east_doorway"), Aabb::fromMinMax({4.63f, 0.0f, -2.8f}, {4.65f, 2.3f, -1.2f})));
    CHECK(east.contains("south"));
    // In the thinnest wall allowed the stop fills the depth of the opening, still inside the wall.
    const std::map<std::string, Aabb> thin = byName(buildOk("room_shell", {{"wallThickness", 0.02}}));
    CHECK(closeTo(thin.at("south_doorway"), Aabb::fromMinMax({-0.8f, 0.0f, 5.0f}, {0.8f, 2.3f, 5.02f})));

    // A doorway onto a hall (I4 step 3) drops the stop and nothing else: the walker passes the door rectangle.
    const ProceduralPrefab open = buildOk("room_shell", {{"doorway", {{"wall", "south"}, {"stop", false}}}});
    std::map<std::string, Aabb> openWalls = byName(open);
    CHECK(openWalls.size() == 6);
    CHECK_FALSE(openWalls.contains("south_doorway"));
    std::map<std::string, Aabb> stopped = walls;
    stopped.erase("south_doorway");
    CHECK(openWalls == stopped);
    CHECK(open.parts.size() == room.parts.size());
    for (std::size_t i = 0; i < open.parts.size() && i < room.parts.size(); ++i) {
        CHECK(open.parts[i].mesh.vertices.size() == room.parts[i].mesh.vertices.size());
    }
    // "stop": true is the default.
    CHECK(byName(buildOk("room_shell", {{"doorway", {{"wall", "south"}, {"stop", true}}}})) == walls);
}

TEST_CASE("geometry/prefabs: cartridge label sits in a recessed pocket", "[geometry][prefabs][cartridge]") {
    const ProceduralPrefab cartridge = buildOk("cartridge");
    const MeshData& label = part(cartridge, "label").mesh;
    REQUIRE(label.vertices.size() == 4);
    CHECK(label.indices.size() == 6);
    const float labelZ = label.vertices[0].position.z;
    for (const rv::geometry::Vertex& v : label.vertices) {
        CHECK(v.normal == glm::vec3(0.0f, 0.0f, 1.0f));
        CHECK(v.position.z == labelZ);
        if (v.uv0 == glm::vec2(0.0f, 0.0f)) {
            CHECK(v.position.x < 0.0f);
            CHECK(v.position.y > 0.0f);
        }
    }
    CHECK(closeTo(label.bounds.size(), glm::vec3(0.125f - 0.024f, 0.085f - 0.024f, 0.0f)));

    const Socket& s = socket(cartridge, "Label");
    CHECK(closeTo(position(s), glm::vec3(0.0f, 0.0f, labelZ)));
    CHECK(closeTo(axis(s, 2), glm::vec3(0.0f, 0.0f, 1.0f)));
    CHECK(closeTo(s.bounds.size(), label.bounds.size()));

    // The label lies exactly labelInset behind the front, down to the smallest inset, and the pocket floor 0.5 mm
    // deeper: every shell face looking +Z (pocket floor, raised border) stays at least 0.5 mm off the label plane.
    struct Recess {
        json params;
        float front;
        float inset;
    };
    for (const Recess& r : {Recess{nullptr, 0.01f, 0.001f}, Recess{{{"labelInset", 0.0005}}, 0.01f, 0.0005f},
                            Recess{{{"labelInset", 0.005}, {"gripRidges", false}}, 0.01f, 0.005f},
                            Recess{{{"size", {0.05, 0.04, 0.004}},
                                    {"chamfer", 0.001},
                                    {"labelMargin", {0.006, 0.006}},
                                    {"labelInset", 0.001}},
                                   0.002f,
                                   0.001f}}) {
        INFO(r.params.dump());
        const ProceduralPrefab prefab = buildOk("cartridge", r.params);
        checkPrefab(prefab);
        const float z = part(prefab, "label").mesh.vertices[0].position.z;
        CHECK(z == Approx(r.front - r.inset).margin(1e-7));
        CHECK(position(socket(prefab, "Label")).z == z);
        const float pocket = z - 0.0005f;
        const MeshData& shell = part(prefab, "plastic_shell").mesh;
        bool pocketFloor = false;
        bool border = false;
        for (std::size_t t = 0; t < shell.triangleCount(); ++t) {
            const rv::test::TriangleRef tri = rv::test::triangleAt(shell, t);
            if (glm::normalize(tri.cross()).z > 0.999f) {
                const float faceZ = tri.v0.position.z;
                pocketFloor = pocketFloor || std::fabs(faceZ - pocket) < 1e-6f;
                border = border || std::fabs(faceZ - r.front) < 1e-6f;
                CHECK(std::fabs(faceZ - z) >= 0.0005f - 1e-6f);
            }
        }
        CHECK(pocketFloor);
        CHECK(border);
    }

    const ProceduralPrefab smooth = buildOk("cartridge", {{"gripRidges", false}});
    CHECK(part(smooth, "plastic_shell").mesh.triangleCount() < part(cartridge, "plastic_shell").mesh.triangleCount());
}

TEST_CASE("geometry/prefabs: furniture and decor sockets", "[geometry][prefabs][sockets]") {
    const ProceduralPrefab cabinet = buildOk("tv_cabinet");
    CHECK(socket(cabinet, "Top").transform == glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.55f, 0.0f)));
    const Socket& shelfSocket = socket(cabinet, "ConsoleShelf");
    const glm::vec3 floor = position(shelfSocket);
    CHECK(floor.y > 0.08f);
    CHECK(floor.y < 0.15f);
    // The compartment is open at the front and holds a default console (0.24 x 0.07 x 0.2).
    const glm::vec3 free = shelfSocket.bounds.size();
    CHECK(free.x > 0.24f);
    CHECK(free.y > 0.07f);
    CHECK(free.z > 0.2f);
    const rv::core::Ray probe{floor + glm::vec3(0.0f, 0.5f * free.y, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    CHECK(probe.origin.z - firstHit(cabinet, probe) <= floor.z + shelfSocket.bounds.min.z + 1e-4f);
    CHECK(part(cabinet, "metal_trim").mesh.bounds.min.y == Approx(0.0f).margin(1e-6));
    const ProceduralPrefab noLegs = buildOk("tv_cabinet", {{"legHeight", 0.0}, {"drawers", 0}});
    CHECK(part(noLegs, "metal_trim").mesh.bounds.min.y > 0.05f);
    // Legs too short to build leave no gap under the carcass either.
    CHECK(buildOk("tv_cabinet", {{"legHeight", 5e-5}}).bounds.min.y == 0.0f);
    CHECK(part(noLegs, "wood_secondary").mesh.triangleCount() < part(cabinet, "wood_secondary").mesh.triangleCount());

    const ProceduralPrefab console = buildOk("console_cartridge");
    const Socket& slot = socket(console, "CartridgeSlot");
    CHECK(position(slot).y == Approx(0.07f));
    CHECK(closeTo(axis(slot, 1), glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(slot.bounds.size().y == 0.0f);
    CHECK(slot.bounds.size().x > 0.13f); // a default cartridge (0.125 x 0.020) fits
    CHECK(slot.bounds.size().z > 0.025f);
    const Socket& front = socket(console, "Front");
    CHECK(closeTo(axis(front, 2), glm::vec3(0.0f, 0.0f, 1.0f)));
    CHECK(position(front).z > 0.095f);

    const ProceduralPrefab poster = buildOk("poster_frame");
    const Socket& picture = socket(poster, "Picture");
    const MeshData& image = part(poster, "picture").mesh;
    REQUIRE(image.vertices.size() == 4);
    CHECK(position(picture).z > 0.0f);
    CHECK(position(picture).z < 0.02f);
    CHECK(image.vertices[0].position.z == position(picture).z);
    CHECK(closeTo(picture.bounds.size(), glm::vec3(0.5f - 0.06f, 0.7f - 0.06f, 0.0f)));
    for (const rv::geometry::Vertex& v : image.vertices) {
        CHECK(v.normal == glm::vec3(0.0f, 0.0f, 1.0f));
        CHECK(v.uv0 == glm::vec2(v.position.x < 0.0f ? 0.0f : 1.0f, v.position.y > 0.0f ? 0.0f : 1.0f));
    }

    const ProceduralPrefab rug = buildOk("rug");
    for (const rv::geometry::Vertex& v : part(rug, "fabric").mesh.vertices) {
        CHECK(v.normal.y > -0.5f); // no bottom face
        if (v.normal == glm::vec3(0.0f, 1.0f, 0.0f)) {
            CHECK(v.uv0.x == Approx((v.position.x + 1.2f) / 2.4f).margin(1e-6));
            CHECK(v.uv0.y == Approx((v.position.z + 0.8f) / 1.6f).margin(1e-6));
        }
    }

    const ProceduralPrefab lamp = buildOk("floor_lamp");
    const glm::vec3 bulb = position(socket(lamp, "LightSource"));
    CHECK(bulb.x == 0.0f);
    CHECK(bulb.z == 0.0f);
    CHECK(bulb.y > 1.6f - 0.3f);
    CHECK(bulb.y < 1.6f);
    const MeshData& shade = part(lamp, "lamp_shade").mesh;
    CHECK(shade.bounds.min.y == Approx(1.3f));
    CHECK(shade.bounds.max.y == Approx(1.6f));
    CHECK(closeTo(shade.bounds.max, glm::vec3(0.22f, 1.6f, 0.22f)));
    // Visible from inside and outside: as many wall triangles face the axis as face away from it, and no two
    // surfaces coincide, so the shade also renders correctly with culling disabled.
    std::size_t inward = 0;
    std::size_t outward = 0;
    for (std::size_t t = 0; t < shade.triangleCount(); ++t) {
        const rv::test::TriangleRef tri = rv::test::triangleAt(shade, t);
        const glm::vec3 n = glm::normalize(tri.cross());
        if (std::fabs(n.y) < 0.9f) {
            if (glm::dot(n, glm::vec3(tri.centroid().x, 0.0f, tri.centroid().z)) < 0.0f) {
                ++inward;
            } else {
                ++outward;
            }
        }
    }
    CHECK(inward > 0);
    CHECK(inward == outward);
    CHECK(rv::test::coplanarOverlaps({&shade}, 0.001f, true) == 0);
}

TEST_CASE("geometry/prefabs: no coincident surfaces face the same way", "[geometry][prefabs][zfighting]") {
    const std::vector<std::pair<std::string_view, json>> variants{
        {"room_shell", nullptr},
        {"room_shell", {{"doorway", {{"wall", "east"}, {"offset", 2.0}}}}},
        {"room_shell", {{"doorway", {{"wall", "north"}, {"offset", -3.0}, {"width", 1.0}}}, {"ceiling", false}}},
        {"shelf", nullptr},
        {"shelf", {{"back", false}, {"plinthHeight", 0.0}, {"levels", 3}}},
        {"cartridge", nullptr},
        {"cartridge", {{"gripRidges", false}, {"labelInset", 0.0005}}},
        {"crt", nullptr},
        {"crt", {{"screenInset", 0.0}, {"backTaper", 0.0}}},
        {"crt", {{"width", 0.3}, {"height", 0.25}, {"depth", 0.15}, {"screenInset", 0.005}, {"backTaper", 0.8}}},
        {"tv_cabinet", nullptr},
        {"tv_cabinet", {{"drawers", 0}, {"legHeight", 0.0}}},
        {"console_cartridge", nullptr},
        {"poster_frame", nullptr},
        {"rug", nullptr},
        {"floor_lamp", nullptr},
        {"armchair", nullptr},
        {"armchair", {{"legHeight", 0.0}, {"width", 1.4}}},
        {"room_shell", {{"wainscotHeight", 1.0}}},
        {"room_shell", {{"wainscotHeight", 0.9}, {"baseboardHeight", 0.0}, {"doorway", {{"wall", "east"}}}}},
        {"cartridge", {{"spineLabel", true}}},
        {"cartridge", {{"spineLabel", true}, {"gripRidges", false}, {"labelInset", 0.0005}}},
        {"shelf", {{"slotFrontInset", 0.01}, {"displayLevels", {1}}}},
        {"display_case", nullptr},
        {"display_case", {{"levels", 1}, {"tiltDeg", 0.0}}},
        {"display_case", {{"levels", 3}, {"levelRise", 0.0}, {"tiltDeg", 45.0}}},
        {"wall_shelf", nullptr},
        {"wall_shelf", {{"tiltDeg", 0.0}, {"ledges", 1}}},
        {"storage_box", nullptr},
        {"storage_box", {{"seed", 7}, {"width", 0.3}}},
        {"magazine_stack", nullptr},
        {"magazine_stack", {{"seed", 7}, {"width", 1.2}}},
        {"bookends", nullptr},
        {"bookends", {{"seed", 7}}},
    };
    for (const auto& [kind, params] : variants) {
        INFO(std::string(kind) + " " + params.dump());
        const ProceduralPrefab prefab = buildOk(kind, params);
        CHECK(rv::test::coplanarOverlaps(meshes(prefab)) == 0);
    }
}

TEST_CASE("geometry/prefabs: JSON overrides and nested defaults", "[geometry][prefabs][json]") {
    // null, {} and the typed builder with default parameters give identical geometry.
    const auto hashes = [](const ProceduralPrefab& prefab) {
        std::vector<std::uint64_t> list;
        for (const PrefabPart& p : prefab.parts) {
            list.push_back(rv::geometry::hashMesh(p.mesh));
        }
        return list;
    };
    const std::vector<std::pair<std::string_view, rv::Expected<ProceduralPrefab>>> typed{
        {"room_shell", rv::geometry::buildRoomShell()},
        {"shelf", rv::geometry::buildShelf()},
        {"cartridge", rv::geometry::buildCartridge()},
        {"crt", rv::geometry::buildCrt()},
        {"tv_cabinet", rv::geometry::buildTvCabinet()},
        {"console_cartridge", rv::geometry::buildConsoleCartridge()},
        {"poster_frame", rv::geometry::buildPosterFrame()},
        {"rug", rv::geometry::buildRug()},
        {"floor_lamp", rv::geometry::buildFloorLamp()},
        {"display_case", rv::geometry::buildDisplayCase()},
        {"wall_shelf", rv::geometry::buildWallShelf()},
        {"storage_box", rv::geometry::buildStorageBox()},
        {"magazine_stack", rv::geometry::buildMagazineStack()},
        {"bookends", rv::geometry::buildBookends()},
    };
    for (const auto& [kind, prefab] : typed) {
        INFO(kind);
        REQUIRE(prefab.has_value());
        CHECK(hashes(*prefab) == hashes(buildOk(kind, nullptr)));
        CHECK(hashes(*prefab) == hashes(buildOk(kind, json::object())));
    }

    CHECK(buildOk("shelf", {{"levels", 3}}).sockets.size() == 3 * (8 + 1));
    const ProceduralPrefab wide = buildOk("crt", {{"screenAspect", 16.0 / 9.0}});
    const glm::vec3 screen = part(wide, "screen").mesh.bounds.size();
    CHECK(screen.x / screen.y == Approx(16.0 / 9.0).epsilon(1e-4));

    // Nested doorway: {} keeps every default, single keys override one value, null removes it.
    const ProceduralPrefab defaults = buildOk("room_shell", {{"doorway", json::object()}});
    CHECK(hashes(defaults) == hashes(buildOk("room_shell")));
    const ProceduralPrefab narrow = buildOk("room_shell", {{"doorway", {{"wall", "west"}, {"width", 1.0}}}});
    const Socket& door = socket(narrow, "Doorway");
    CHECK(door.bounds.size().x == Approx(1.0f));
    CHECK(door.bounds.size().y == Approx(2.3f));
    CHECK(position(door).x < 0.0f);
    CHECK(buildOk("room_shell", {{"doorway", nullptr}}).sockets.empty());
    // Integers are accepted where numbers are expected.
    CHECK(closeTo(buildOk("rug", {{"width", 3}, {"depth", 2}}).bounds.size(), glm::vec3(3.0f, 0.012f, 2.0f)));
}

TEST_CASE("geometry/prefabs: JSON errors name the parameter", "[geometry][prefabs][json]") {
    using rv::ErrorCode;
    struct Case {
        std::string_view kind;
        json params;
        ErrorCode code;
        std::string context;
    };
    const std::vector<Case> cases{
        // Unknown keys (the first in key order is reported).
        {"shelf", {{"bogus", 1}}, ErrorCode::InvalidArgument, "shelf.bogus"},
        {"shelf", {{"zeta", 1}, {"alpha", 2}}, ErrorCode::InvalidArgument, "shelf.alpha"},
        {"room_shell", {{"doorway", {{"color", "red"}}}}, ErrorCode::InvalidArgument, "room_shell.doorway.color"},
        // Wrong types.
        {"shelf", {{"levels", "five"}}, ErrorCode::InvalidArgument, "shelf.levels"},
        {"shelf", {{"levels", 2.5}}, ErrorCode::InvalidArgument, "shelf.levels"},
        {"shelf", {{"back", 1}}, ErrorCode::InvalidArgument, "shelf.back"},
        {"shelf", {{"width", nullptr}}, ErrorCode::InvalidArgument, "shelf.width"},
        {"shelf", {{"slotSize", {0.1, 0.2}}}, ErrorCode::InvalidArgument, "shelf.slotSize"},
        {"shelf", {{"slotSize", {0.1, "a", 0.2}}}, ErrorCode::InvalidArgument, "shelf.slotSize"},
        {"room_shell", {{"doorway", 5}}, ErrorCode::InvalidArgument, "room_shell.doorway"},
        {"room_shell", {{"doorway", {{"wall", "up"}}}}, ErrorCode::InvalidArgument, "room_shell.doorway.wall"},
        {"room_shell", {{"doorway", {{"wall", 3}}}}, ErrorCode::InvalidArgument, "room_shell.doorway.wall"},
        {"shelf", json::array({1, 2}), ErrorCode::InvalidArgument, "shelf"},
        {"crt", "big", ErrorCode::InvalidArgument, "crt"},
        // Out of range or inconsistent values.
        {"shelf", {{"levels", 0}}, ErrorCode::OutOfRange, "shelf.levels"},
        {"shelf", {{"levels", 9999999999LL}}, ErrorCode::OutOfRange, "shelf.levels"},
        {"shelf", {{"levels", 60}}, ErrorCode::OutOfRange, "shelf.levels"},
        {"shelf", {{"width", -1.0}}, ErrorCode::OutOfRange, "shelf.width"},
        {"shelf", {{"slotSize", {0.125, 0.5, 0.02}}}, ErrorCode::OutOfRange, "shelf.levels"},
        {"shelf", {{"slotSize", {0.125, 1.95, 0.02}}}, ErrorCode::OutOfRange, "shelf.slotSize"},
        {"shelf", {{"slotSize", {0.125, 0.085, 0.4}}}, ErrorCode::OutOfRange, "shelf.slotSize"},
        {"shelf", {{"slotSize", {1.5, 0.085, 0.02}}}, ErrorCode::OutOfRange, "shelf.slotSize"},
        // No free height at all: the plinth is blamed, or the boards when there is no plinth.
        {"shelf", {{"height", 0.3}, {"plinthHeight", 0.5}}, ErrorCode::OutOfRange, "shelf.plinthHeight"},
        {"shelf",
         {{"height", 0.2}, {"boardThickness", 0.1}, {"plinthHeight", 0.0}},
         ErrorCode::OutOfRange,
         "shelf.boardThickness"},
        {"crt", {{"screenAspect", 10.0}}, ErrorCode::OutOfRange, "crt.screenAspect"},
        {"crt", {{"depth", 0.15}, {"screenInset", 0.05}}, ErrorCode::OutOfRange, "crt.screenInset"},
        {"room_shell", {{"size", {9.0, 0.1}}}, ErrorCode::OutOfRange, "room_shell.size"},
        {"room_shell", {{"doorway", {{"width", 8.9}}}}, ErrorCode::OutOfRange, "room_shell.doorway.width"},
        {"room_shell", {{"doorway", {{"offset", 4.0}}}}, ErrorCode::OutOfRange, "room_shell.doorway.offset"},
        {"room_shell", {{"doorway", {{"height", 2.95}}}}, ErrorCode::OutOfRange, "room_shell.doorway.height"},
        {"cartridge", {{"labelMargin", {0.001, 0.012}}}, ErrorCode::OutOfRange, "cartridge.labelMargin"},
        {"cartridge", {{"labelInset", 0.0}}, ErrorCode::OutOfRange, "cartridge.labelInset"},
        {"tv_cabinet", {{"drawers", 8}}, ErrorCode::OutOfRange, "tv_cabinet.drawers"},
        {"tv_cabinet", {{"legHeight", 0.45}}, ErrorCode::OutOfRange, "tv_cabinet.legHeight"},
        {"console_cartridge", {{"height", 0.5}}, ErrorCode::OutOfRange, "console_cartridge.height"},
        {"poster_frame", {{"frameWidth", 0.3}}, ErrorCode::OutOfRange, "poster_frame.frameWidth"},
        {"rug", {{"thickness", 0.5}}, ErrorCode::OutOfRange, "rug.thickness"},
        {"floor_lamp", {{"shadeHeight", 0.9}}, ErrorCode::OutOfRange, "floor_lamp.shadeHeight"},
        {"armchair", {{"seat", 0.4}}, ErrorCode::InvalidArgument, "armchair.seat"},
        {"armchair", {{"width", 3.0}}, ErrorCode::OutOfRange, "armchair.width"},
        {"armchair", {{"seatHeight", 0.6}, {"armHeight", 0.62}}, ErrorCode::OutOfRange, "armchair.armHeight"},
        {"armchair", {{"armHeight", 0.82}}, ErrorCode::OutOfRange, "armchair.height"},
        {"armchair", {{"seatHeight", 0.3}, {"legHeight", 0.2}}, ErrorCode::OutOfRange, "armchair.legHeight"},
        {"room_shell", {{"wainscotHeight", -0.5}}, ErrorCode::OutOfRange, "room_shell.wainscotHeight"},
        {"room_shell", {{"wainscotHeight", 0.15}}, ErrorCode::OutOfRange, "room_shell.wainscotHeight"},
        {"room_shell", {{"wainscotHeight", 2.9}}, ErrorCode::OutOfRange, "room_shell.wainscotHeight"},
        // The collection's shelves (I3 step 1b).
        {"shelf", {{"slotFrontInset", "front"}}, ErrorCode::InvalidArgument, "shelf.slotFrontInset"},
        {"shelf", {{"slotFrontInset", -0.01}}, ErrorCode::OutOfRange, "shelf.slotFrontInset"},
        {"shelf", {{"slotFrontInset", 0.33}}, ErrorCode::OutOfRange, "shelf.slotFrontInset"},
        {"shelf", {{"displayLevels", 4}}, ErrorCode::InvalidArgument, "shelf.displayLevels"},
        {"shelf", {{"displayLevels", {1, 2.5}}}, ErrorCode::InvalidArgument, "shelf.displayLevels"},
        {"shelf", {{"displayLevels", {5}}}, ErrorCode::OutOfRange, "shelf.displayLevels"},
        {"shelf", {{"displayLevels", {-1}}}, ErrorCode::OutOfRange, "shelf.displayLevels"},
        {"shelf", {{"displayLevels", {2, 1, 2}}}, ErrorCode::OutOfRange, "shelf.displayLevels"},
        {"shelf",
         {{"displayLevels", {1}}, {"displaySlotSize", {0.125, 0.5, 0.02}}},
         ErrorCode::OutOfRange,
         "shelf.levels"},
        {"shelf",
         {{"displayLevels", {1}}, {"displaySlotSize", {1.5, 0.085, 0.02}}},
         ErrorCode::OutOfRange,
         "shelf.displaySlotSize"},
        // An unused display size is never checked against the shelf.
        {"shelf", {{"displaySlotGap", -1.0}}, ErrorCode::OutOfRange, "shelf.displaySlotGap"},
        {"cartridge", {{"spineLabel", "yes"}}, ErrorCode::InvalidArgument, "cartridge.spineLabel"},
        {"cartridge",
         {{"spineLabel", true}, {"spineLabelSize", {0.016, 0.08}}},
         ErrorCode::OutOfRange,
         "cartridge.spineLabelSize"},
        {"cartridge",
         {{"spineLabel", true}, {"spineLabelSize", {0.01, 0.2}}},
         ErrorCode::OutOfRange,
         "cartridge.spineLabelSize"},
        // With a spine the top border must keep 0.5 mm above the 0.5 mm the shell gives up.
        {"cartridge",
         {{"spineLabel", true}, {"labelMargin", {0.012, 0.0025}}},
         ErrorCode::OutOfRange,
         "cartridge.labelMargin"},
        {"display_case", {{"levels", 5}}, ErrorCode::OutOfRange, "display_case.levels"},
        {"display_case", {{"levels", 4}, {"depth", 0.3}}, ErrorCode::OutOfRange, "display_case.levels"},
        {"display_case", {{"baseHeight", 0.8}}, ErrorCode::OutOfRange, "display_case.baseHeight"},
        // Level 0 above the glass top blames the base, a later level the rise.
        {"display_case",
         {{"levels", 1}, {"slotSize", {0.125, 0.4, 0.02}}},
         ErrorCode::OutOfRange,
         "display_case.baseHeight"},
        {"display_case", {{"levelRise", 0.3}}, ErrorCode::OutOfRange, "display_case.levelRise"},
        {"display_case", {{"slotSize", {0.125, 0.085, 0.6}}}, ErrorCode::OutOfRange, "display_case.slotSize"},
        {"display_case", {{"width", 0.4}, {"slotSize", {0.4, 0.085, 0.02}}}, ErrorCode::OutOfRange,
         "display_case.slotSize"},
        {"display_case", {{"tiltDeg", 75.0}}, ErrorCode::OutOfRange, "display_case.tiltDeg"},
        {"wall_shelf", {{"ledges", 0}}, ErrorCode::OutOfRange, "wall_shelf.ledges"},
        {"wall_shelf", {{"ledges", 3}}, ErrorCode::OutOfRange, "wall_shelf.ledges"},
        {"wall_shelf",
         {{"height", 0.15}, {"ledges", 1}, {"slotSize", {0.125, 0.12, 0.02}}},
         ErrorCode::OutOfRange,
         "wall_shelf.height"},
        {"wall_shelf", {{"depth", 0.05}}, ErrorCode::OutOfRange, "wall_shelf.depth"},
        {"wall_shelf", {{"slotSize", {0.9, 0.085, 0.02}}}, ErrorCode::OutOfRange, "wall_shelf.slotSize"},
        {"wall_shelf", {{"mountHeight", 3.0}}, ErrorCode::OutOfRange, "wall_shelf.mountHeight"},
        {"storage_box", {{"width", 0.05}}, ErrorCode::OutOfRange, "storage_box.width"},
        {"magazine_stack", {{"seed", -1}}, ErrorCode::OutOfRange, "magazine_stack.seed"},
        {"bookends", {{"height", 2.0}}, ErrorCode::OutOfRange, "bookends.height"},
        {"bookends", {{"shelf", 1}}, ErrorCode::InvalidArgument, "bookends.shelf"},
    };
    for (const Case& c : cases) {
        const rv::Error error = buildError(c.kind, c.params);
        INFO(error.toString());
        CHECK(error.code == c.code);
        CHECK(error.context == c.context);
    }

    const auto unknown = rv::geometry::buildPrefab("spaceship", nullptr);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    CHECK(unknown.error().context == "spaceship");
    CHECK(unknown.error().message.find("room_shell") != std::string::npos);
}

TEST_CASE("geometry/prefabs: typed builders reject invalid parameters", "[geometry][prefabs][robustness]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    rv::geometry::ShelfParams shelf;
    shelf.width = nan;
    const auto badShelf = rv::geometry::buildShelf(shelf);
    REQUIRE_FALSE(badShelf.has_value());
    CHECK(badShelf.error().code == rv::ErrorCode::OutOfRange);
    CHECK(badShelf.error().context == "shelf.width");

    rv::geometry::CrtParams crt;
    crt.depth = inf;
    REQUIRE_FALSE(rv::geometry::buildCrt(crt).has_value());
    CHECK(rv::geometry::buildCrt(crt).error().context == "crt.depth");

    rv::geometry::RoomShellParams room;
    room.doorway->wall = static_cast<rv::geometry::WallSide>(42);
    REQUIRE_FALSE(rv::geometry::buildRoomShell(room).has_value());
    CHECK(rv::geometry::buildRoomShell(room).error().context == "room_shell.doorway.wall");
    room.doorway.reset();
    room.size.x = nan;
    CHECK(rv::geometry::buildRoomShell(room).error().context == "room_shell.size");

    rv::geometry::CartridgeParams cartridge;
    cartridge.labelMargin = {0.07f, 0.012f}; // wider than half the cartridge
    CHECK(rv::geometry::buildCartridge(cartridge).error().context == "cartridge.labelMargin");

    rv::geometry::FloorLampParams lamp;
    lamp.baseRadius = 0.0f;
    CHECK(rv::geometry::buildFloorLamp(lamp).error().context == "floor_lamp.baseRadius");

    rv::geometry::ArmchairParams chair;
    chair.depth = nan;
    CHECK(rv::geometry::buildArmchair(chair).error().context == "armchair.depth");
}

TEST_CASE("geometry/prefabs: identical parameters give identical prefabs", "[geometry][prefabs][determinism]") {
    for (const std::string_view kind : rv::geometry::prefabKinds()) {
        const ProceduralPrefab first = buildOk(kind);
        const ProceduralPrefab second = buildOk(kind);
        INFO(kind);
        REQUIRE(first.parts.size() == second.parts.size());
        for (std::size_t i = 0; i < first.parts.size(); ++i) {
            CHECK(rv::geometry::hashMesh(first.parts[i].mesh) == rv::geometry::hashMesh(second.parts[i].mesh));
        }
        REQUIRE(first.sockets.size() == second.sockets.size());
        for (std::size_t i = 0; i < first.sockets.size(); ++i) {
            CHECK(first.sockets[i].id == second.sockets[i].id);
            CHECK(first.sockets[i].transform == second.sockets[i].transform);
            CHECK(first.sockets[i].bounds == second.sockets[i].bounds);
        }
        CHECK(first.bounds == second.bounds);
    }
}

TEST_CASE("geometry/prefabs: parameter sweeps stay valid and keep their bounds", "[geometry][prefabs][sweep]") {
    for (const double width : {0.4, 1.2, 2.5}) {
        for (const int levels : {1, 4, 7}) {
            for (const bool back : {true, false}) {
                const json params{
                    {"width", width}, {"levels", levels}, {"back", back}, {"plinthHeight", back ? 0.1 : 0.0}};
                INFO(params.dump());
                const ProceduralPrefab shelf = buildOk("shelf", params);
                checkPrefab(shelf);
                CHECK(closeTo(shelf.bounds.size(), glm::vec3(static_cast<float>(width), 2.0f, 0.35f)));
                CHECK(shelf.sockets.size() % static_cast<std::size_t>(levels) == 0);
            }
        }
    }
    for (const double aspect : {1.0, 1.3333333, 2.4}) {
        for (const double taper : {0.0, 0.35, 0.8}) {
            for (const double inset : {0.0, 0.012, 0.03}) {
                for (const json& size : {json{{"width", 0.71}, {"height", 0.61}, {"depth", 0.56}},
                                         json{{"width", 0.36}, {"height", 0.31}, {"depth", 0.33}}}) {
                    json params = size;
                    params["screenAspect"] = aspect;
                    params["backTaper"] = taper;
                    params["screenInset"] = inset;
                    INFO(params.dump());
                    const ProceduralPrefab crt = buildOk("crt", params);
                    checkPrefab(crt);
                    const glm::vec3 expected(size["width"].get<float>(), size["height"].get<float>(),
                                             size["depth"].get<float>());
                    CHECK(closeTo(crt.bounds.size(), expected));
                    const glm::vec3 screen = part(crt, "screen").mesh.bounds.size();
                    CHECK(screen.x / screen.y == Approx(aspect).epsilon(1e-4));
                }
            }
        }
    }
    for (const std::string wall : {"north", "south", "east", "west"}) {
        for (const double offset : {-1.5, 0.0, 2.0}) {
            const json params{{"size", {6.0, 7.0}}, {"doorway", {{"wall", wall}, {"offset", offset}}}};
            INFO(params.dump());
            const ProceduralPrefab room = buildOk("room_shell", params);
            checkPrefab(room);
            const Socket& door = socket(room, "Doorway");
            const glm::vec3 outward = axis(door, 2);
            const glm::vec3 inside = position(door) - 2.0f * outward + glm::vec3(0.0f, 1.0f, 0.0f);
            CHECK(rayHits(room, rv::core::Ray{inside, outward}, 10.0f) == 0);
        }
    }
    for (const int drawers : {0, 1, 3, 5}) {
        for (const double legs : {0.0, 0.12}) {
            const json params{{"drawers", drawers}, {"legHeight", legs}, {"height", 0.7}};
            INFO(params.dump());
            const ProceduralPrefab cabinet = buildOk("tv_cabinet", params);
            checkPrefab(cabinet);
            CHECK(closeTo(cabinet.bounds.size(), glm::vec3(1.2f, 0.7f, 0.5f)));
        }
    }
    for (const json& params : {json{{"width", 0.12}, {"height", 0.03}, {"depth", 0.1}},
                               json{{"width", 0.5}, {"height", 0.12}, {"depth", 0.1}},
                               json{{"width", 1.0}, {"height", 0.3}, {"depth", 1.0}}}) {
        INFO(params.dump());
        const ProceduralPrefab console = buildOk("console_cartridge", params);
        checkPrefab(console);
        CHECK(rv::test::coplanarOverlaps(meshes(console)) == 0);
        CHECK(closeTo(console.bounds.size(), glm::vec3(params["width"].get<float>(), params["height"].get<float>(),
                                                       params["depth"].get<float>())));
    }
    for (const json& params : {json{{"height", 0.5}, {"shadeHeight", 0.05}, {"shadeRadius", 0.05}},
                               json{{"height", 3.0}, {"shadeHeight", 1.0}, {"baseRadius", 1.0}}}) {
        INFO(params.dump());
        checkPrefab(buildOk("floor_lamp", params));
    }
    for (const json& params : {json{{"width", 0.5}, {"depth", 0.5}, {"height", 0.5}, {"seatHeight", 0.3},
                                     {"armHeight", 0.4}, {"legHeight", 0.05}},
                               json{{"width", 2.5}, {"depth", 1.5}, {"height", 1.4}, {"seatHeight", 0.7},
                                     {"armHeight", 1.0}, {"legHeight", 0.3}}}) {
        INFO(params.dump());
        const ProceduralPrefab chair = buildOk("armchair", params);
        checkPrefab(chair);
        CHECK(rv::test::coplanarOverlaps(meshes(chair)) == 0);
        CHECK(closeTo(chair.bounds.size(), glm::vec3(params["width"].get<float>(), params["height"].get<float>(),
                                                     params["depth"].get<float>())));
        CHECK(position(socket(chair, "Seat")).y == Approx(params["seatHeight"].get<float>()));
    }
}

TEST_CASE("geometry/prefabs: the plant stands within its spread, its tallest leaf at its height", "[geometry][prefabs]") {
    for (const int seed : {0, 7, 12345}) {
        const json params{{"height", 1.05}, {"potRadius", 0.15}, {"potHeight", 0.3},
                          {"spread", 0.19}, {"leaves", 14},      {"seed", seed}};
        const ProceduralPrefab plant = buildOk("plant", params);
        INFO("seed " << seed);
        checkPrefab(plant);
        CHECK(plant.bounds.max.y == Approx(1.05f).margin(1e-3f)); // a leaning tip's back face, within a millimetre
        CHECK(plant.bounds.min.y == Approx(0.0f).margin(1e-6f));
        for (const float extent : {plant.bounds.min.x, plant.bounds.min.z, -plant.bounds.max.x, -plant.bounds.max.z}) {
            CHECK(extent >= -0.19f - 1e-5f);
        }
        // Exact, symmetric footprint: the saucer as wide as the leaves' reach.
        CHECK(plant.bounds.min.x == Approx(-0.19f));
        CHECK(plant.bounds.max.z == Approx(0.19f));
    }
    // Another seed, another rosette.
    CHECK(rv::geometry::hashMesh(part(buildOk("plant", {{"seed", 1}}), "leaves").mesh) !=
          rv::geometry::hashMesh(part(buildOk("plant", {{"seed", 2}}), "leaves").mesh));
    CHECK_FALSE(rv::geometry::buildPrefab("plant", {{"spread", 0.1}}).has_value()); // narrower than its pot
    // A saucer of its own (I6 step 2's halls): as wide as asked, in its role, the pot's role the pot alone; the bounds
    // within the spread. Narrower than the pot or wider than the spread: refused.
    const json saucered{{"height", 1.15}, {"potRadius", 0.17}, {"potHeight", 0.34},
                        {"spread", 0.3},  {"leaves", 16},      {"saucerRadius", 0.196}};
    const ProceduralPrefab plant = buildOk("plant", saucered);
    checkPrefab(plant);
    const rv::geometry::MeshData& saucer = part(plant, "saucer").mesh;
    CHECK(saucer.bounds.max.x == Approx(0.196f));
    CHECK(saucer.bounds.max.y == Approx(0.018f));
    CHECK(part(plant, "pot").mesh.bounds.max.x < 0.19f);
    CHECK(plant.bounds.max.x <= 0.3f + 1e-5f);
    json defaults = saucered;
    defaults.erase("saucerRadius");
    CHECK(rv::geometry::hashMesh(part(buildOk("plant", defaults), "leaves").mesh) ==
          rv::geometry::hashMesh(part(plant, "leaves").mesh));
    CHECK(part(buildOk("plant", defaults), "pot").mesh.bounds.max.x == Approx(0.3f));
    for (const double radius : {0.1, 0.35, -0.2}) {
        json bad = saucered;
        bad["saucerRadius"] = radius;
        CHECK_FALSE(rv::geometry::buildPrefab("plant", bad).has_value());
    }
}

TEST_CASE("geometry/primitives: defaults, overrides and errors", "[geometry][primitives]") {
    struct Expectation {
        std::string_view kind;
        glm::vec3 size;
        std::size_t vertices;
    };
    for (const Expectation& e :
         {Expectation{"box", {1.0f, 1.0f, 1.0f}, 24}, Expectation{"plane", {1.0f, 0.0f, 1.0f}, 4},
          Expectation{"quad", {1.0f, 1.0f, 0.0f}, 4}, Expectation{"sphere", {1.0f, 1.0f, 1.0f}, 2 * 32 + 15 * 33},
          Expectation{"cylinder", {1.0f, 1.0f, 1.0f}, 4 * 32 + 4}}) {
        INFO(e.kind);
        for (const json& params : {json(nullptr), json::object()}) {
            const auto mesh = rv::geometry::buildPrimitive(e.kind, params);
            REQUIRE(mesh.has_value());
            CHECK(mesh->name == e.kind);
            CHECK(mesh->vertices.size() == e.vertices);
            CHECK(rv::geometry::validateMesh(*mesh).has_value());
            CHECK(rv::test::windingMismatches(*mesh) == 0);
            CHECK(closeTo(mesh->bounds.size(), e.size));
            CHECK(closeTo(mesh->bounds.center(), glm::vec3(0.0f)));
        }
    }

    const auto box =
        rv::geometry::buildPrimitive("box", {{"size", {2.0, 3.0, 4.0}}, {"uv", "meters"}, {"metersPerTile", 0.5}});
    REQUIRE(box.has_value());
    CHECK(closeTo(box->bounds.size(), glm::vec3(2.0f, 3.0f, 4.0f)));
    for (const rv::geometry::Vertex& v : box->vertices) {
        if (v.normal == glm::vec3(0.0f, 0.0f, 1.0f)) {
            CHECK(v.uv0.x == Approx(v.position.x / 0.5f));
            CHECK(v.uv0.y == Approx(-v.position.y / 0.5f));
        }
    }
    // "uvOrigin": the meter layout as if the box stood there; the mesh stays centred on the origin.
    const auto placed = rv::geometry::buildPrimitive(
        "box", {{"size", {2.0, 3.0, 4.0}}, {"uv", "meters"}, {"metersPerTile", 0.5}, {"uvOrigin", {10.0, 1.5, -2.0}}});
    REQUIRE(placed.has_value());
    CHECK(closeTo(placed->bounds.min, glm::vec3(-1.0f, -1.5f, -2.0f)));
    REQUIRE(placed->vertices.size() == box->vertices.size());
    for (std::size_t i = 0; i < placed->vertices.size(); ++i) {
        const rv::geometry::Vertex& v = placed->vertices[i];
        CHECK(closeTo(v.position, box->vertices[i].position));
        if (v.normal == glm::vec3(0.0f, 0.0f, 1.0f)) {
            CHECK(v.uv0.x == Approx((v.position.x + 10.0f) / 0.5f));
            CHECK(v.uv0.y == Approx(-(v.position.y + 1.5f) / 0.5f));
        }
    }
    // "grain": u along each face's longer side (a batten standing up: the grain runs up its front).
    const auto batten = rv::geometry::buildPrimitive("box", {{"size", {0.04, 0.8, 0.016}}, {"uv", "grain"}});
    REQUIRE(batten.has_value());
    glm::vec2 uvMin(1e9f);
    glm::vec2 uvMax(-1e9f);
    for (const rv::geometry::Vertex& v : batten->vertices) {
        if (v.normal == glm::vec3(0.0f, 0.0f, 1.0f)) {
            uvMin = glm::min(uvMin, v.uv0);
            uvMax = glm::max(uvMax, v.uv0);
        }
    }
    CHECK(uvMax.x - uvMin.x == Approx(0.8f));
    CHECK(uvMax.y - uvMin.y == Approx(0.04f));
    const auto plane = rv::geometry::buildPrimitive("plane", {{"size", {4.0, 2.0}}, {"uvTiling", {4.0, 2.0}}});
    REQUIRE(plane.has_value());
    float maxU = 0.0f;
    for (const rv::geometry::Vertex& v : plane->vertices) {
        CHECK(v.normal == glm::vec3(0.0f, 1.0f, 0.0f));
        maxU = std::max(maxU, v.uv0.x);
    }
    CHECK(maxU == 4.0f);
    const auto sphere = rv::geometry::buildPrimitive("sphere", {{"radius", 2.0}, {"segments", 8}, {"rings", 4}});
    REQUIRE(sphere.has_value());
    CHECK(sphere->vertices.size() == 2 * 8 + 3 * 9);
    CHECK(sphere->bounds.max.y == Approx(2.0f));
    const auto open = rv::geometry::buildPrimitive("cylinder", {{"capped", false}, {"segments", 12}});
    REQUIRE(open.has_value());
    CHECK(open->vertices.size() == 2 * 13);

    struct Failure {
        std::string_view kind;
        json params;
        rv::ErrorCode code;
        std::string context;
    };
    for (const Failure& f : {
             Failure{"box", {{"color", "red"}}, rv::ErrorCode::InvalidArgument, "box.color"},
             Failure{"box", {{"uv", "planar"}}, rv::ErrorCode::InvalidArgument, "box.uv"},
             Failure{"box", {{"size", {1.0, 1.0}}}, rv::ErrorCode::InvalidArgument, "box.size"},
             Failure{"box", {{"size", {1.0, 0.0, 1.0}}}, rv::ErrorCode::OutOfRange, "box.size"},
             Failure{"plane", {{"uvTiling", {0.0, 1.0}}}, rv::ErrorCode::OutOfRange, "plane.uvTiling"},
             Failure{"quad", {{"size", "large"}}, rv::ErrorCode::InvalidArgument, "quad.size"},
             Failure{"sphere", {{"segments", 8.5}}, rv::ErrorCode::InvalidArgument, "sphere.segments"},
             Failure{"sphere", {{"segments", 2}}, rv::ErrorCode::OutOfRange, "sphere.segments"},
             Failure{"sphere", {{"rings", 1000}}, rv::ErrorCode::OutOfRange, "sphere.rings"},
             Failure{"cylinder", {{"radius", 0.0}}, rv::ErrorCode::OutOfRange, "cylinder.radius"},
             Failure{"cylinder", {{"capped", "yes"}}, rv::ErrorCode::InvalidArgument, "cylinder.capped"},
         }) {
        const auto mesh = rv::geometry::buildPrimitive(f.kind, f.params);
        INFO(std::string(f.kind) + " " + f.params.dump());
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == f.code);
        CHECK(mesh.error().context == f.context);
        CHECK_FALSE(mesh.error().message.empty());
    }
    const auto unknown = rv::geometry::buildPrimitive("torus", nullptr);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == rv::ErrorCode::NotFound);
    CHECK(unknown.error().context == "torus");
}

TEST_CASE("geometry/prefabs: the era furniture's lights, seats and refusals (I6 step 3b)", "[geometry][prefabs]") {
    // The banker's lamp's bulb 0.34 m above its stand, about where a floor lamp's stands (1.45 m) on a 1.11 m stand.
    const ProceduralPrefab banker = buildOk("banker_lamp", {{"standHeight", 1.11}});
    CHECK(position(socket(banker, "LightSource")).y == Approx(1.45f).margin(1e-4f));
    CHECK(position(socket(buildOk("floor_lamp"), "LightSource")).y == Approx(1.454f).margin(1e-4f));
    // The torchiere's bulb in its bowl, below the rim.
    const ProceduralPrefab torchiere = buildOk("floor_lamp", {{"shade", "torchiere"}, {"shadeHeight", 0.16}});
    CHECK(position(socket(torchiere, "LightSource")).y == Approx(1.6f - 0.6f * 0.16f).margin(1e-4f));
    CHECK(position(socket(buildOk("sofa"), "Seat")).y == Approx(0.42f));
    CHECK(position(socket(buildOk("bar_stool"), "Seat")).y == Approx(0.76f));
    // The desk's screen faces the sitter, above the top.
    const ProceduralPrefab desk = buildOk("desk");
    const Socket& screen = socket(desk, "Screen");
    CHECK(position(screen).y > 0.75f);
    CHECK(screen.bounds.size().x == Approx(0.27f));

    const auto refused = [](std::string_view kind, const json& params) {
        auto built = rv::geometry::buildPrefab(kind, params);
        REQUIRE_FALSE(built.has_value());
        return built.error().context;
    };
    CHECK(refused("floor_lamp", {{"shade", "cone"}}) == "floor_lamp.shade");
    CHECK(refused("sofa", {{"seats", 5}}) == "sofa.seats");
    CHECK(refused("sofa", {{"armHeight", 0.44}}) == "sofa.armHeight");
    CHECK(refused("desk", {{"depth", 0.55}}) == "desk.depth"); // too shallow for the computer
    CHECK(buildOk("desk", {{"depth", 0.55}, {"computer", false}}).bounds.size().z == Approx(0.55f));
    CHECK(refused("office_chair", {{"height", 0.7}, {"seatHeight", 0.5}}) == "office_chair.height");
    CHECK(refused("bar_stool", {{"height", 1.2}}) == "bar_stool.height");
    CHECK(refused("banker_lamp", {{"standWidth", 0.3}, {"shadeLength", 0.35}}) == "banker_lamp.shadeLength");
    CHECK(refused("showcase", {{"height", 0.8}, {"baseHeight", 0.6}}) == "showcase.baseHeight");
    CHECK(refused("showcase", {{"item", "jukebox"}}) == "showcase.item");
    CHECK(refused("beanbag", {{"height", 0.2}}) == "beanbag.height");

    // The step's review: a one-seat sofa from 0.7 m (the handheld archive's lounge chair), the showcase's big boxes
    // with their covers, its lit strip, and a vitrine too small for its item refused.
    CHECK(buildOk("sofa", {{"width", 0.7}, {"seats", 1}, {"pillows", false}, {"legHeight", 0.2}}).bounds.size().x ==
          Approx(0.7f));
    CHECK(refused("sofa", {{"width", 0.65}, {"seats", 1}}) == "sofa.width");
    const ProceduralPrefab boxes = buildOk("showcase", {{"item", "boxes"}, {"boardLight", true}});
    CHECK(rv::geometry::findPart(boxes, "cover") != nullptr);
    CHECK(rv::geometry::findPart(boxes, "plastic_body") != nullptr);
    CHECK(rv::geometry::findPart(boxes, "board_light") != nullptr);
    CHECK(part(boxes, "cover").mesh.vertices.size() == 3u * 4u); // three boxes, a quad each
    CHECK(boxes.bounds == buildOk("showcase").bounds);
    CHECK(rv::geometry::findPart(buildOk("showcase"), "board_light") == nullptr);
    CHECK(rv::geometry::findPart(buildOk("showcase"), "cover") == nullptr);
    CHECK(refused("showcase", {{"height", 0.7}, {"baseHeight", 0.4}, {"item", "boxes"}}) == "showcase.item");
    CHECK(buildOk("showcase", {{"height", 0.7}, {"baseHeight", 0.4}, {"item", "none"}}).bounds.size().y ==
          Approx(0.7f));
    // The banker's lamp stands on a card catalogue: 3 x 6 drawer fronts (chamfered boxes of 96 vertices).
    CHECK(part(buildOk("banker_lamp"), "wood_secondary").mesh.vertices.size() == 18u * 96u);
}

TEST_CASE("geometry/prefabs: the hub's landmark, totem, column and railing (I6 step 5)", "[geometry][prefabs][hub]") {
    // The landmark: within its radius, its globe above the pedestal at the socket, its consoles counted.
    const ProceduralPrefab landmark = buildOk("hub_landmark");
    CHECK(landmark.bounds.min.x == Approx(-2.1f).margin(1e-4));
    CHECK(landmark.bounds.max.z == Approx(2.1f).margin(1e-4));
    CHECK(landmark.bounds.min.y == Approx(0.0f).margin(1e-5));
    const Socket& globe = socket(landmark, "Globe");
    const glm::vec3 centre(globe.transform[3]);
    CHECK(centre.x == Approx(0.0f).margin(1e-5));
    CHECK(centre.y > 2.0f);
    CHECK(landmark.bounds.max.y == Approx(centre.y + 0.55f + 0.06f + 0.014f).margin(0.01));
    CHECK(part(landmark, "globe").mesh.bounds.max.y <= centre.y + 0.56f);
    CHECK(rv::geometry::findPart(buildOk("hub_landmark", {{"consoles", 0}}), "plastic_body") == nullptr);
    CHECK(part(buildOk("hub_landmark", {{"consoles", 12}}), "plastic_body").mesh.vertices.size() >
          part(landmark, "plastic_body").mesh.vertices.size());
    CHECK(buildError("hub_landmark", {{"globeRadius", 1.2}}).context == "hub_landmark.globeRadius");
    CHECK(buildError("hub_landmark", {{"consoles", 13}}).context == "hub_landmark.consoles");
    // I6 step 5's review: larger consoles, and a riser for a boxed game between them, each a socket on its top.
    const ProceduralPrefab displays = buildOk("hub_landmark", {{"consoles", 9}, {"consoleScale", 1.4}, {"displays", 9}});
    CHECK(displays.sockets.size() == 1 + 9);
    const glm::vec3 riser(socket(displays, "Display_0").transform[3]);
    CHECK(riser.y > 0.3f + 0.38f);
    CHECK(std::abs(riser.z) > 1.0f);
    CHECK(part(displays, "plastic_body").mesh.bounds.max.y >
          part(buildOk("hub_landmark", {{"consoles", 9}}), "plastic_body").mesh.bounds.max.y);
    CHECK(buildError("hub_landmark", {{"consoleScale", 3.0}}).context == "hub_landmark.consoleScale");
    CHECK(buildError("hub_landmark", {{"displays", 13}}).context == "hub_landmark.displays");
    // The totem: its screen and sign quads on its front, the screen's socket at its centre facing out.
    const ProceduralPrefab totem = buildOk("info_totem");
    const Socket& screen = socket(totem, "Screen");
    CHECK(glm::vec3(screen.transform[3]).z > 0.1f);
    CHECK(glm::vec3(screen.transform[3]).z < 0.16f);
    CHECK(part(totem, "screen").mesh.bounds.size().x == Approx(0.46f).margin(1e-4));
    CHECK(part(totem, "sign").mesh.bounds.max.y < 1.75f);
    CHECK(buildError("info_totem", {{"screenWidth", 0.55}}).context == "info_totem.screenWidth");
    // The column: the plinth its footprint, the capital's abacus within it; refused too thin a plinth.
    const ProceduralPrefab column = buildOk("column", {{"height", 4.0}});
    CHECK(column.bounds.max.y == Approx(4.0f).margin(1e-4));
    CHECK(part(column, "wood_secondary").mesh.bounds.max.x <= 0.28f + 1e-4f);
    CHECK(buildError("column", {{"plinthSize", 0.3}}).context == "column.plinthSize");
    // The railing: posts at both ends at most postPitch apart, balusters between them.
    const ProceduralPrefab railing = buildOk("railing", {{"length", 6.0}, {"postPitch", 1.5}});
    CHECK(railing.bounds.size().x == Approx(6.0f).margin(1e-4));
    CHECK(part(railing, "metal").mesh.vertices.size() > 0);
    CHECK(buildError("railing", {{"length", 0.1}}).context == "railing.length");
}

TEST_CASE("geometry/prefabs: the arcade cabinets' slots and the hero cabinet's screen (I6 step 6)",
          "[geometry][prefabs][arcade]") {
    // A pair: one face-out slot per cabinet, the screen item's volume, at each cabinet's middle, leaning back and set
    // into its bezel, the level's anchor spanning both screens.
    const ProceduralPrefab pair = buildOk("arcade_cabinet");
    const std::vector<rv::geometry::SlotLevel> levels = rv::geometry::slotLevels(pair.sockets);
    REQUIRE(levels.size() == 1);
    CHECK(levels.front().level == 0);
    CHECK(levels.front().count == 2);
    CHECK(closeTo(levels.front().slotSize, glm::vec3(0.3f, 0.4f, 0.006f)));
    const Socket& left = socket(pair, "Slot_0_0");
    const Socket& right = socket(pair, "Slot_0_1");
    CHECK(position(left).x == Approx(-0.23f).margin(1e-5));
    CHECK(position(right).x == Approx(0.23f).margin(1e-5));
    CHECK(position(left).y == Approx(0.86f + 0.05f * std::cos(glm::radians(8.0f))).margin(2e-3));
    const glm::vec3 out(left.transform[2]);
    CHECK(out.z == Approx(std::cos(glm::radians(8.0f))).margin(1e-5));
    CHECK(out.y == Approx(std::sin(glm::radians(8.0f))).margin(1e-5));
    CHECK(position(left).z < 0.2f - 0.1f);
    const Socket& level = socket(pair, "Level_0");
    CHECK(level.bounds.min.x == Approx(-0.38f).margin(1e-4));
    CHECK(level.bounds.max.x == Approx(0.38f).margin(1e-4));
    CHECK(pair.bounds.contains(level.bounds));
    // The marquees and the paints alternate: one quad of each marquee role, the pair's two bodies apart.
    CHECK(part(pair, "marquee").mesh.bounds.max.x < 0.0f);
    CHECK(part(pair, "marquee_alt").mesh.bounds.min.x > 0.0f);
    CHECK(part(pair, "body").mesh.bounds.max.x < 0.0f);
    CHECK(part(pair, "body_alt").mesh.bounds.min.x > 0.0f);
    CHECK(rv::geometry::findPart(pair, "screen") == nullptr);
    // Four in a row; one alone has no alternate roles.
    CHECK(buildOk("arcade_cabinet", {{"count", 4}}).sockets.size() == 1 + 4);
    const ProceduralPrefab single = buildOk("arcade_cabinet", {{"count", 1}});
    CHECK(rv::geometry::findPart(single, "body_alt") == nullptr);
    CHECK(rv::geometry::findPart(single, "marquee_alt") == nullptr);
    // The hero cabinet: no slot, a screen quad carrying the ScreenSurface, facing out of the leaning bezel.
    const ProceduralPrefab hero = buildOk(
        "arcade_cabinet", {{"count", 1}, {"width", 0.7}, {"height", 1.85}, {"depth", 0.85}, {"slots", false},
                           {"screenSize", {0.5, 0.375}}, {"screenTiltDeg", 12.0}, {"marqueeHeight", 0.24}});
    CHECK(rv::geometry::slotLevels(hero.sockets).empty());
    const Socket& surface = socket(hero, "ScreenSurface");
    CHECK(surface.kind == SocketKind::ScreenSurface);
    REQUIRE(surface.meshIndex.has_value());
    CHECK(hero.parts[*surface.meshIndex].materialRole == "screen");
    CHECK(part(hero, "screen").mesh.bounds.size().x == Approx(0.5f).margin(1e-4));
    CHECK(position(surface).y > 1.0f);
    CHECK(position(surface).y < 1.5f);
    CHECK(rv::geometry::findPart(hero, "screen_back") == nullptr);
    // Refusals: a screen wider than the cabinet, taller than the panel and the marquee leave, a tilt through its back.
    CHECK(buildError("arcade_cabinet", {{"screenSize", {0.4, 0.4}}}).context == "arcade_cabinet.screenSize");
    CHECK(buildError("arcade_cabinet", {{"screenSize", {0.3, 0.8}}}).context == "arcade_cabinet.screenSize");
    CHECK(buildError("arcade_cabinet", {{"depth", 0.3}, {"screenTiltDeg", 25.0}, {"screenSize", {0.3, 0.3}}}).context ==
          "arcade_cabinet.screenTiltDeg");
    CHECK(buildError("arcade_cabinet", {{"count", 5}}).context == "arcade_cabinet.count");
}

TEST_CASE("geometry/prefabs: the handhelds' counter showcase keeps the display case's slots and bounds (I6 step 6)",
          "[geometry][prefabs][display_case]") {
    const json params{{"slotSize", {0.057, 0.065, 0.008}}};
    const ProceduralPrefab plain = buildOk("display_case", params);
    json counterParams = params;
    counterParams["counter"] = true;
    counterParams["boardLight"] = true;
    const ProceduralPrefab counter = buildOk("display_case", counterParams);
    // The same bounds and slots: a look, never a plan fact.
    CHECK(closeTo(counter.bounds, plain.bounds));
    REQUIRE(counter.sockets.size() == plain.sockets.size());
    for (std::size_t i = 0; i < plain.sockets.size(); ++i) {
        INFO(plain.sockets[i].id);
        CHECK(counter.sockets[i].id == plain.sockets[i].id);
        CHECK(counter.sockets[i].transform == plain.sockets[i].transform);
        CHECK(counter.sockets[i].bounds == plain.sockets[i].bounds);
    }
    // Handhelds on the deck of the glass base, under the vitrine's floor; the lit strips; no door.
    const PrefabPart& handhelds = part(counter, "plastic_body");
    CHECK(handhelds.mesh.bounds.max.y < 0.52f);
    CHECK(handhelds.mesh.bounds.min.y > 0.06f);
    CHECK(handhelds.mesh.bounds.size().x > 0.6f);
    CHECK(rv::geometry::findPart(counter, "board_light") != nullptr);
    CHECK(rv::geometry::findPart(plain, "board_light") == nullptr);
    CHECK(rv::geometry::findPart(plain, "plastic_body") == nullptr);
    CHECK(rv::geometry::findPart(counter, "metal_trim") == nullptr); // no doors, no knobs
    // Lit alone, the vitrine's strips only.
    const ProceduralPrefab lit = buildOk("display_case", {{"boardLight", true}});
    CHECK(part(lit, "board_light").mesh.bounds.min.y > 0.9f);
    // A counter of a pack without the light has no strip at all (I6 step 6's review: not under its base's top either).
    json unlitParams = counterParams;
    unlitParams["boardLight"] = false;
    const ProceduralPrefab unlit = buildOk("display_case", unlitParams);
    CHECK(rv::geometry::findPart(unlit, "board_light") == nullptr);
    CHECK(rv::geometry::findPart(unlit, "plastic_body") != nullptr);
}

TEST_CASE("geometry/prefabs: the review of I6 step 6: side art, lit handheld screens, a cover per box, a light column",
          "[geometry][prefabs][arcade]") {
    // The cabinets' side art: on the outer sides of the row's first and last cabinets only, within the row's bounds,
    // a few millimetres off the side panels; neither the slots nor the bounds move.
    const ProceduralPrefab plain = buildOk("arcade_cabinet");
    const ProceduralPrefab art = buildOk("arcade_cabinet", {{"sideArt", true}});
    CHECK(rv::geometry::findPart(plain, "side_art") == nullptr);
    const MeshData& sides = part(art, "side_art").mesh;
    CHECK(closeTo(art.bounds, plain.bounds));
    CHECK(art.sockets.size() == plain.sockets.size());
    CHECK(sides.bounds.min.x == Approx(plain.bounds.min.x + 0.001f).margin(1e-4));
    CHECK(sides.bounds.max.x == Approx(plain.bounds.max.x - 0.001f).margin(1e-4));
    CHECK(sides.bounds.min.y > 0.05f);
    CHECK(sides.bounds.max.y < 1.7f - 0.2f);
    // Two quads per side, their normals facing out of the row; the picture's uv spans 0..1 across both.
    std::size_t outward = 0;
    float uMin = 1.0f;
    float uMax = 0.0f;
    float vMin = 1.0f;
    float vMax = 0.0f;
    for (const rv::geometry::Vertex& v : sides.vertices) {
        outward += (v.position.x < 0.0f && v.normal.x < -0.99f) || (v.position.x > 0.0f && v.normal.x > 0.99f) ? 1 : 0;
        uMin = std::min(uMin, v.uv0.x);
        uMax = std::max(uMax, v.uv0.x);
        vMin = std::min(vMin, v.uv0.y);
        vMax = std::max(vMax, v.uv0.y);
    }
    CHECK(outward == sides.vertices.size());
    CHECK(sides.triangleCount() == 2 * 2 * 2);
    CHECK(uMin == Approx(0.0f).margin(1e-5));
    CHECK(uMax == Approx(1.0f).margin(1e-5));
    CHECK(vMin == Approx(0.0f).margin(1e-5));
    CHECK(vMax == Approx(1.0f).margin(1e-5));
    CHECK(part(buildOk("arcade_cabinet", {{"sideArt", true}, {"count", 1}}), "side_art").mesh.triangleCount() == 8);

    // A showcase's handheld lit in "screen", its boxes each a cover of its own.
    const ProceduralPrefab handheld = buildOk("showcase", {{"item", "handheld"}, {"litScreen", true}});
    CHECK(rv::geometry::findPart(handheld, "screen") != nullptr);
    CHECK(rv::geometry::findPart(buildOk("showcase", {{"item", "handheld"}}), "screen") == nullptr);
    const ProceduralPrefab boxes = buildOk("showcase", {{"item", "boxes"}, {"coverPerBox", true}});
    CHECK(part(boxes, "cover").mesh.triangleCount() == 2);
    CHECK(part(boxes, "cover_2").mesh.triangleCount() == 2);
    CHECK(part(boxes, "cover_3").mesh.triangleCount() == 2);
    CHECK(part(buildOk("showcase", {{"item", "boxes"}}), "cover").mesh.triangleCount() == 6);

    // A counter's handhelds lit: one display each, on the deck.
    const json counter{{"slotSize", {0.057, 0.065, 0.008}}, {"counter", true}, {"litScreens", true}};
    const ProceduralPrefab lit = buildOk("display_case", counter);
    const MeshData& screens = part(lit, "screen").mesh;
    CHECK(screens.triangleCount() >= 2 * 4); // a quad per handheld, four at least on a 1.2 m counter
    CHECK(screens.triangleCount() % 2 == 0);
    CHECK(screens.bounds.max.y < part(lit, "plastic_body").mesh.bounds.max.y);

    // A light column: a lit tube on the base, the bulb where a drum lamp's would be, within the lamp's bounds.
    const ProceduralPrefab drum =
        buildOk("floor_lamp", {{"height", 1.5}, {"shadeRadius", 0.17}, {"shadeHeight", 0.26}});
    const ProceduralPrefab column =
        buildOk("floor_lamp", {{"height", 1.5}, {"shadeRadius", 0.17}, {"shadeHeight", 0.26}, {"shade", "tube"}});
    CHECK(position(socket(column, "LightSource")) == position(socket(drum, "LightSource")));
    const MeshData& tube = part(column, "lamp_shade").mesh;
    CHECK(tube.bounds.min.y < 0.1f);
    CHECK(tube.bounds.max.y > 1.4f);
    CHECK(tube.bounds.size().x < 0.07f);
    CHECK(drum.bounds.contains(column.bounds));
}

TEST_CASE("geometry/prefabs: the secondary TV, a sideboard carrying a small CRT (I5 step 3)",
          "[geometry][prefabs][secondary_tv]") {
    // The default: the narrow cabinet's footprint (0.8 x 0.36 m), the CRT's 0.4 m on its 0.5 m top.
    const ProceduralPrefab tv = buildOk("secondary_tv");
    checkPrefab(tv);
    CHECK(rv::test::coplanarOverlaps(meshes(tv)) == 0);
    // The screen is the CRT's own (one consumer CRT model in two sizes, style §15.1): a 4:3 picture on top of the
    // sideboard, facing +Z, its socket naming the screen part.
    const Socket& surface = socket(tv, "ScreenSurface");
    CHECK(surface.kind == SocketKind::ScreenSurface);
    REQUIRE(surface.meshIndex.has_value());
    CHECK(tv.parts[*surface.meshIndex].materialRole == "screen");
    const glm::vec3 screen = part(tv, "screen").mesh.bounds.size();
    CHECK(screen.x / screen.y == Approx(4.0 / 3.0).epsilon(1e-4));
    CHECK(position(surface).y > 0.5f);
    CHECK(position(surface).y < 0.9f);
    CHECK(axis(surface, 2).z == Approx(1.0f));
    // The same screen as a CRT of that size standing on the top, its front 2 cm behind the top's front edge.
    const ProceduralPrefab crt = buildOk("crt", {{"width", 0.46}, {"height", 0.4}, {"depth", 0.32}, {"bezel", 0.04}});
    const glm::vec3 lifted = position(socket(crt, "ScreenSurface")) + glm::vec3(0.0f, 0.5f, 0.18f - 0.02f - 0.16f);
    CHECK(closeTo(position(surface), lifted));
    CHECK(part(tv, "screen").mesh.vertices.size() == part(crt, "screen").mesh.vertices.size());
    CHECK(position(socket(tv, "TV_Stand")).y == Approx(0.5f));
    // Moved across the top, wider and deeper; the CRT's back within the top.
    const ProceduralPrefab offset = buildOk(
        "secondary_tv", {{"width", 1.2}, {"depth", 0.5}, {"tvSize", {0.5, 0.42, 0.36}}, {"tvOffset", -0.3}});
    CHECK(position(socket(offset, "ScreenSurface")).x == Approx(-0.3f).margin(1e-5));
    CHECK(closeTo(offset.bounds, Aabb::fromMinMax({-0.6f, 0.0f, -0.25f}, {0.6f, 0.92f, 0.25f})));
    // Refusals, named by this kind's parameters.
    CHECK(buildError("secondary_tv", {{"tvSize", {0.9, 0.4, 0.32}}}).context == "secondary_tv.tvSize");
    CHECK(buildError("secondary_tv", {{"tvSize", {0.46, 0.4, 0.35}}}).context == "secondary_tv.tvSize");
    CHECK(buildError("secondary_tv", {{"tvOffset", 0.2}}).context == "secondary_tv.tvOffset");
    CHECK(buildError("secondary_tv", {{"tvBezel", 0.2}}).context == "secondary_tv.tvBezel");
    CHECK(buildError("secondary_tv", {{"drawers", 9}}).context == "secondary_tv.drawers");
    CHECK(buildError("secondary_tv", {{"screen", 1}}).context == "secondary_tv.screen");
}
