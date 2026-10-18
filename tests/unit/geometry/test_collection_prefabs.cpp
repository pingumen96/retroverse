// The collection's prefabs of I3 step 1b (ADR-0010 §3-§5, ADR-0011 §4): the cartridge's spine, how items stand in
// slots, the shelf's front inset and display levels, the display case, the featured wall shelf and the shelf dressing.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
using rv::geometry::SlotFacing;
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

glm::vec3 position(const glm::mat4& m) {
    return glm::vec3(m[3]);
}

glm::vec3 axis(const glm::mat4& m, int column) {
    return glm::vec3(m[column]);
}

bool closeTo(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f) {
    return rv::core::nearlyEqual(a, b, epsilon);
}

std::vector<const MeshData*> meshes(const ProceduralPrefab& prefab) {
    std::vector<const MeshData*> list;
    for (const PrefabPart& p : prefab.parts) {
        list.push_back(&p.mesh);
    }
    return list;
}

std::vector<std::uint64_t> hashes(const ProceduralPrefab& prefab) {
    std::vector<std::uint64_t> list;
    for (const PrefabPart& p : prefab.parts) {
        list.push_back(rv::geometry::hashMesh(p.mesh));
    }
    return list;
}

/// Distance to the closest surface of the parts with a role in `roles` (every part when empty) along `ray`.
float firstHit(const ProceduralPrefab& prefab, const rv::core::Ray& ray, const std::set<std::string>& roles = {}) {
    float best = std::numeric_limits<float>::infinity();
    for (const PrefabPart& p : prefab.parts) {
        if (!roles.empty() && !roles.contains(p.materialRole)) {
            continue;
        }
        for (std::size_t t = 0; t < p.mesh.triangleCount(); ++t) {
            const rv::test::TriangleRef tri = rv::test::triangleAt(p.mesh, t);
            if (const auto hit = rv::test::intersect(ray, tri.v0.position, tri.v1.position, tri.v2.position)) {
                best = std::min(best, *hit);
            }
        }
    }
    return best;
}

/// The eight corners of a socket's slot volume, in the prefab.
std::vector<glm::vec3> slotCorners(const Socket& s) {
    std::vector<glm::vec3> corners;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local((i & 1) ? s.bounds.max.x : s.bounds.min.x, (i & 2) ? s.bounds.max.y : s.bounds.min.y,
                              (i & 4) ? s.bounds.max.z : s.bounds.min.z);
        corners.push_back(glm::vec3(s.transform * glm::vec4(local, 1.0f)));
    }
    return corners;
}

/// Valid parts, the overall bounds the union of the parts and, unless `coplanar` is false (large props: the check
/// compares every pair of triangles), no coplanar overlaps facing the same way.
void checkValid(const ProceduralPrefab& prefab, bool coplanar = true) {
    INFO(prefab.kind);
    REQUIRE_FALSE(prefab.parts.empty());
    Aabb unionOfParts;
    for (const PrefabPart& p : prefab.parts) {
        INFO(p.mesh.name);
        CHECK(rv::geometry::validateMesh(p.mesh).has_value());
        CHECK(rv::test::windingMismatches(p.mesh) == 0);
        CHECK(rv::test::nonUnitNormals(p.mesh) == 0);
        unionOfParts.expand(p.mesh.bounds);
    }
    CHECK(prefab.bounds == unionOfParts);
    if (coplanar) {
        CHECK(rv::test::coplanarOverlaps(meshes(prefab)) == 0);
    }
}

/// The shelf of the collection's modules (furniture.shelf.*_module.generic): spine-out levels, optionally a display level.
json moduleParams(bool heroBand) {
    json params{{"width", 0.9},
                {"height", 1.775},
                {"depth", 0.24},
                {"levels", 7},
                {"boardThickness", 0.025},
                {"back", true},
                {"plinthHeight", 0.35},
                {"slotSize", {0.02, 0.125, 0.085}},
                {"slotGap", 0.004},
                {"slotMargin", 0.03},
                {"slotFrontInset", 0.01}};
    if (heroBand) {
        params["displayLevels"] = {5};
        params["displaySlotSize"] = {0.125, 0.085, 0.02};
        params["displaySlotGap"] = 0.006;
        params["displaySlotMargin"] = 0.025;
    }
    return params;
}

constexpr glm::vec3 kCartridge{0.125f, 0.085f, 0.020f};

} // namespace

TEST_CASE("geometry/collection: the cartridge's spine is an end label on its top face", "[geometry][collection]") {
    const ProceduralPrefab plain = buildOk("cartridge");
    CHECK(rv::geometry::findPart(plain, "spine") == nullptr);
    CHECK(rv::geometry::findSocket(plain.sockets, "Spine") == nullptr);

    const ProceduralPrefab spined = buildOk("cartridge", {{"spineLabel", true}});
    checkValid(spined);
    // The front label is the same quad with or without a spine.
    CHECK(rv::geometry::hashMesh(part(spined, "label").mesh) == rv::geometry::hashMesh(part(plain, "label").mesh));
    CHECK(spined.bounds == plain.bounds);

    // The quad: 13.5 x 86.4 mm (the 40 x 256 spine texture's aspect), on the nominal top, the shell 0.5 mm below.
    const MeshData& spine = part(spined, "spine").mesh;
    REQUIRE(spine.vertices.size() == 4);
    const glm::vec3 size = spine.bounds.size();
    CHECK(size.x == Approx(0.0864f));
    CHECK(size.z == Approx(0.0135f));
    CHECK(size.z / size.x == Approx(40.0f / 256.0f));
    for (const rv::geometry::Vertex& v : spine.vertices) {
        CHECK(closeTo(v.normal, glm::vec3(0.0f, 1.0f, 0.0f)));
        CHECK(v.position.y == Approx(0.0425f));
    }
    CHECK(part(spined, "plastic_shell").mesh.bounds.max.y == Approx(0.0425f - 0.0005f));
    // Inside the flat top: between the chamfers along X, behind the front pocket (1.5 mm) and the chamfers across Z.
    CHECK(spine.bounds.min.x >= -0.0625f + 0.002f);
    CHECK(spine.bounds.max.x <= 0.0625f - 0.002f);
    CHECK(spine.bounds.min.z >= -0.01f + 0.002f - 1e-6f);
    CHECK(spine.bounds.max.z <= 0.01f - 0.0015f - 0.002f + 1e-6f);

    // The socket: centered on the quad, facing +Y, bounds the label rectangle in its frame.
    const Socket& s = socket(spined, "Spine");
    CHECK(s.kind == SocketKind::Other);
    CHECK(closeTo(position(s.transform), spine.bounds.center()));
    CHECK(closeTo(axis(s.transform, 2), glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(closeTo(rv::geometry::socketSize(s), glm::vec3(0.0135f, 0.0864f, 0.0f)));

    // Standing spine-out, the spine faces the slot's +Z, its +X end up, and the picture's top-left corner (uv 0, 0) is
    // the top-left corner seen from the front: a spine texture is drawn upright.
    const glm::mat4 stand = rv::geometry::slotItemTransform(SlotFacing::SpineOut, kCartridge);
    CHECK(closeTo(glm::mat3(stand) * axis(s.transform, 2), glm::vec3(0.0f, 0.0f, 1.0f)));
    CHECK(closeTo(glm::mat3(stand) * axis(s.transform, 1), glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(closeTo(glm::mat3(stand) * axis(s.transform, 0), glm::vec3(1.0f, 0.0f, 0.0f)));
    for (const rv::geometry::Vertex& v : spine.vertices) {
        const glm::vec3 p = glm::vec3(stand * glm::vec4(v.position, 1.0f));
        const glm::vec3 c = glm::vec3(stand * glm::vec4(spine.bounds.center(), 1.0f));
        INFO(v.uv0.x << ", " << v.uv0.y);
        CHECK((p.x < c.x) == (v.uv0.x < 0.5f)); // u grows to the right
        CHECK((p.y > c.y) == (v.uv0.y < 0.5f)); // v grows downwards
        CHECK(p.z == Approx(0.0425f)); // on the slot's front face
    }
}

TEST_CASE("geometry/collection: items fill their slots face-out and spine-out", "[geometry][collection]") {
    CHECK(rv::geometry::slotSizeFor(SlotFacing::FaceOut, kCartridge) == kCartridge);
    CHECK(rv::geometry::slotSizeFor(SlotFacing::SpineOut, kCartridge) == glm::vec3(0.02f, 0.125f, 0.085f));
    const Aabb item = Aabb::fromMinMax(-0.5f * kCartridge, 0.5f * kCartridge);
    for (const SlotFacing facing : {SlotFacing::FaceOut, SlotFacing::SpineOut}) {
        const glm::mat4 transform = rv::geometry::slotItemTransform(facing, kCartridge);
        const glm::mat3 rotation(transform);
        CHECK(glm::determinant(rotation) == Approx(1.0f));
        CHECK(closeTo(glm::transpose(rotation) * rotation[0], glm::vec3(1.0f, 0.0f, 0.0f)));
        const glm::vec3 slot = rv::geometry::slotSizeFor(facing, kCartridge);
        const Aabb expected = Aabb::fromMinMax(glm::vec3(-0.5f * slot.x, 0.0f, -0.5f * slot.z),
                                               glm::vec3(0.5f * slot.x, slot.y, 0.5f * slot.z));
        const Aabb placed = item.transformed(transform);
        CHECK(closeTo(placed.min, expected.min));
        CHECK(closeTo(placed.max, expected.max));
    }
    // Face-out the front faces the viewer; spine-out the top face does, the front to the right.
    const glm::mat3 spineOut(rv::geometry::slotItemTransform(SlotFacing::SpineOut, kCartridge));
    CHECK(closeTo(spineOut * glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
    CHECK(closeTo(spineOut * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(closeTo(spineOut * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST_CASE("geometry/collection: slot ids and the slots of each level", "[geometry][collection][slots]") {
    using rv::geometry::parseSlotId;
    REQUIRE(parseSlotId("Slot_0_0").has_value());
    CHECK(parseSlotId("Slot_6_32")->level == 6);
    CHECK(parseSlotId("Slot_6_32")->index == 32);
    for (const std::string_view bad : {"Slot_", "Slot_1", "Slot_1_", "Slot__1", "Slot_01_2", "Slot_1_02", "Slot_-1_2",
                                       "Slot_1_2_3", "Slot_a_1", "slot_1_2", "Label", "Slot_99999999999_0"}) {
        INFO(bad);
        CHECK_FALSE(parseSlotId(bad).has_value());
    }

    const ProceduralPrefab module = buildOk("shelf", moduleParams(true));
    const std::vector<rv::geometry::SlotLevel> levels = rv::geometry::slotLevels(module.sockets);
    REQUIRE(levels.size() == 7);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        INFO("level " << i);
        CHECK(levels[i].level == static_cast<int>(i));
        CHECK(levels[i].count == (i == 5 ? 6 : 33));
        CHECK(closeTo(levels[i].slotSize, i == 5 ? kCartridge : glm::vec3(0.02f, 0.125f, 0.085f)));
        CHECK(levels[i].height == Approx(0.35f + static_cast<float>(i) * 0.2f + 0.025f));
    }
    // Only ShelfSlot sockets with slot ids count.
    std::vector<Socket> sockets = module.sockets;
    for (Socket& s : sockets) {
        if (s.id == "Slot_0_0") {
            s.kind = SocketKind::Other;
        } else if (s.id == "Slot_6_32") {
            s.id = "Slot_x";
        }
    }
    const auto fewer = rv::geometry::slotLevels(sockets);
    CHECK(fewer.front().count == 32);
    CHECK(fewer.back().count == 32);
    CHECK(rv::geometry::slotLevels(buildOk("cartridge").sockets).empty());
}

TEST_CASE("geometry/collection: the modules' slots stand at the front of their boards",
          "[geometry][collection][shelf]") {
    for (const bool band : {false, true}) {
        INFO("hero band " << band);
        const ProceduralPrefab module = buildOk("shelf", moduleParams(band));
        checkValid(module);
        CHECK(closeTo(module.bounds.size(), glm::vec3(0.9f, 1.775f, 0.24f)));
        CHECK(module.sockets.size() == (band ? 6u * 33u + 6u : 7u * 33u) + 7u); // the slots and 7 Level anchors
        for (const Socket& s : module.sockets) {
            INFO(s.id);
            if (const auto level = rv::geometry::parseLevelId(s.id)) {
                // The level's free volume: the inner width, 17.5 cm tall, the board's depth.
                CHECK(position(s.transform).y == Approx(0.35f + static_cast<float>(*level) * 0.2f + 0.025f));
                CHECK(closeTo(rv::geometry::socketSize(s), glm::vec3(0.85f, 0.175f, 0.232f)));
                continue;
            }
            const auto address = rv::geometry::parseSlotId(s.id);
            REQUIRE(address.has_value());
            const bool display = band && address->level == 5;
            const glm::vec3 slot = rv::geometry::socketSize(s);
            CHECK(closeTo(slot, display ? kCartridge : glm::vec3(0.02f, 0.125f, 0.085f)));
            // 1 cm behind the boards' front edge, on its board, under the next one.
            CHECK(position(s.transform).z + 0.5f * slot.z == Approx(0.12f - 0.01f));
            const float board = 0.35f + static_cast<float>(address->level) * 0.2f;
            CHECK(position(s.transform).y == Approx(board + 0.025f));
            CHECK(position(s.transform).y + slot.y < board + 0.2f);
            // An item standing in it touches nothing: the board below and the back only.
            const glm::vec3 center = position(s.transform) + glm::vec3(0.0f, 0.5f * slot.y, 0.0f);
            const rv::core::Ray up{center, glm::vec3(0.0f, 1.0f, 0.0f)};
            CHECK(firstHit(module, up) > 0.5f * slot.y);
        }
        // Spine-out slots 2.4 cm apart, the first 3 cm from the inner side.
        CHECK(position(socket(module, "Slot_0_1").transform).x - position(socket(module, "Slot_0_0").transform).x ==
              Approx(0.024f));
        CHECK(position(socket(module, "Slot_0_0").transform).x == Approx(-0.425f + 0.03f + 0.01f));
        if (band) {
            CHECK(position(socket(module, "Slot_5_1").transform).x -
                      position(socket(module, "Slot_5_0").transform).x ==
                  Approx(0.131f));
            CHECK(rv::geometry::findSocket(module.sockets, "Slot_5_6") == nullptr);
        }
    }
    // Without a front inset the items stay centered in the board depth (the test room's shelves).
    const ProceduralPrefab centered = buildOk("shelf");
    CHECK(position(socket(centered, "Slot_0_0").transform).z == Approx(0.5f * (-0.175f + 0.008f + 0.175f)));
}

TEST_CASE("geometry/collection: the display case shows two levels behind glass", "[geometry][collection][display]") {
    const ProceduralPrefab display = buildOk("display_case");
    checkValid(display);
    const auto levels = rv::geometry::slotLevels(display.sockets);
    REQUIRE(levels.size() == 2);
    CHECK(levels[0].count == 8);
    CHECK(levels[1].count == 8);
    CHECK(levels[1].height > levels[0].height + 0.1f); // the back level stands higher

    // The vitrine's interior: above the base (0.55 m), below the rails (0.92 m), between the glass and the back.
    const Aabb interior = Aabb::fromMinMax(glm::vec3(-0.582f, 0.55f, -0.238f), glm::vec3(0.582f, 0.92f, 0.235f));
    // A standing visitor 0.6 m in front of the case (style 6.2's inspection distance): the front level through the
    // front glass, the back one through the top.
    const glm::vec3 eye(0.0f, 1.65f, 0.85f);
    std::vector<glm::vec3> centers;
    for (const Socket& s : display.sockets) {
        INFO(s.id);
        if (const auto level = rv::geometry::parseLevelId(s.id)) {
            // The level's free volume leans with it and holds its slots.
            CHECK(s.kind == SocketKind::Anchor);
            CHECK(closeTo(axis(s.transform, 1), glm::vec3(0.0f, std::cos(0.5236f), -std::sin(0.5236f)), 1e-4f));
            const glm::mat4 toLevel = glm::inverse(s.transform);
            for (int index = 0; index < 8; ++index) {
                const Socket& slot = socket(display, "Slot_" + std::to_string(*level) + "_" + std::to_string(index));
                for (const glm::vec3& corner : slotCorners(slot)) {
                    CHECK(s.bounds.inflated(1e-5f).contains(glm::vec3(toLevel * glm::vec4(corner, 1.0f))));
                }
            }
            continue;
        }
        CHECK(s.kind == SocketKind::ShelfSlot);
        CHECK(closeTo(rv::geometry::socketSize(s), kCartridge));
        // Leaning back by 30 degrees: the front faces up and forward.
        CHECK(closeTo(axis(s.transform, 1), glm::vec3(0.0f, std::cos(0.5236f), -std::sin(0.5236f)), 1e-4f));
        for (const glm::vec3& corner : slotCorners(s)) {
            CHECK(interior.contains(corner));
        }
        const glm::vec3 center = glm::vec3(s.transform * glm::vec4(0.0f, 0.5f * kCartridge.y, 0.0f, 1.0f));
        const glm::vec3 front =
            glm::vec3(s.transform * glm::vec4(0.0f, 0.5f * kCartridge.y, 0.5f * kCartridge.z, 1.0f));
        centers.push_back(center);
        // Seen from the visitor, the item's front lies behind the glass and nothing else.
        const glm::vec3 toItem = front - eye;
        const rv::core::Ray look{eye, glm::normalize(toItem)};
        const float glass = firstHit(display, look, {"glass"});
        const float other = firstHit(display, look, {"wood_primary", "wood_secondary", "metal_trim", "fabric"});
        CHECK(glass < glm::length(toItem));
        CHECK(other > glm::length(toItem) - 1e-3f);
        // It stands on its ledge and leans on its panel.
        const rv::core::Ray down{glm::vec3(s.transform * glm::vec4(0.0f, 0.001f, 0.0f, 1.0f)), -axis(s.transform, 1)};
        CHECK(firstHit(display, down, {"fabric"}) == Approx(0.001f).margin(1e-4f));
        const rv::core::Ray back{center, -axis(s.transform, 2)};
        CHECK(firstHit(display, back, {"fabric"}) == Approx(0.5f * kCartridge.z).margin(1e-4f));
    }
    // Slots never overlap: neighbours 13.7 cm apart along a level.
    CHECK(glm::length(centers[1] - centers[0]) == Approx(0.137f));
    // A taller case with three levels and another tilt.
    const ProceduralPrefab tall =
        buildOk("display_case", {{"levels", 3}, {"height", 1.1}, {"depth", 0.6}, {"tiltDeg", 20.0}, {"width", 0.9}});
    checkValid(tall);
    CHECK(rv::geometry::slotLevels(tall.sockets).size() == 3);
}

TEST_CASE("geometry/collection: the featured wall shelf leans its items against the back board",
          "[geometry][collection][display]") {
    const ProceduralPrefab shelf = buildOk("wall_shelf");
    checkValid(shelf);
    const auto levels = rv::geometry::slotLevels(shelf.sockets);
    REQUIRE(levels.size() == 2);
    CHECK(levels[0].count == 6);
    CHECK(levels[1].count == 6);
    const float backFace = -0.04f + 0.015f;
    for (const Socket& s : shelf.sockets) {
        INFO(s.id);
        if (const auto level = rv::geometry::parseLevelId(s.id)) {
            // On the ledge, between the back board and the lip, up to the next ledge or the cap.
            const Aabb free = s.bounds.transformed(s.transform);
            CHECK(free.min.y == Approx(1.1f + static_cast<float>(*level) * 0.1675f + 0.018f));
            CHECK(free.max.y == Approx(1.1f + static_cast<float>(*level + 1) * 0.1675f));
            CHECK(free.min.z == Approx(backFace));
            CHECK(free.max.z == Approx(0.04f - 0.008f));
            continue;
        }
        const auto address = rv::geometry::parseSlotId(s.id);
        REQUIRE(address.has_value());
        const float ledgeTop = 1.1f + static_cast<float>(address->level) * 0.1675f + 0.018f;
        float lowest = 10.0f;
        float backmost = 10.0f;
        float frontmost = -10.0f;
        float highest = -10.0f;
        for (const glm::vec3& corner : slotCorners(s)) {
            lowest = std::min(lowest, corner.y);
            highest = std::max(highest, corner.y);
            backmost = std::min(backmost, corner.z);
            frontmost = std::max(frontmost, corner.z);
            CHECK(shelf.bounds.contains(corner));
        }
        CHECK(lowest == Approx(ledgeTop).margin(1e-5f));   // resting on the ledge
        CHECK(backmost == Approx(backFace).margin(1e-5f)); // against the back board
        CHECK(frontmost < 0.04f - 0.008f);                 // behind the lip
        CHECK(highest < ledgeTop - 0.018f + 0.1675f);      // under the next ledge or the cap
        CHECK(closeTo(axis(s.transform, 1), glm::vec3(0.0f, std::cos(0.20944f), -std::sin(0.20944f)), 1e-4f));
    }
}

TEST_CASE("geometry/collection: shelf dressing fills a level's volume and varies with its seed",
          "[geometry][collection][dressing]") {
    for (const std::string_view kind : {"storage_box", "magazine_stack", "bookends"}) {
        std::set<std::vector<std::uint64_t>> variants;
        for (const double width : {0.1, 0.3, 0.85, 1.5, 3.0}) {
            for (const double height : {0.05, 0.17, 0.4}) {
                for (const double depth : {0.05, 0.23, 0.6}) {
                    for (const int seed : {0, 1, 99, 2147483647}) {
                        const json params{{"width", width}, {"height", height}, {"depth", depth}, {"seed", seed}};
                        INFO(std::string(kind) + " " + params.dump());
                        const ProceduralPrefab prop = buildOk(kind, params);
                        checkValid(prop, width <= 0.85 && depth <= 0.23);
                        CHECK(prop.sockets.empty());
                        const Aabb space = Aabb::fromMinMax(
                            glm::vec3(-0.5f * static_cast<float>(width), 0.0f, -0.5f * static_cast<float>(depth)),
                            glm::vec3(0.5f * static_cast<float>(width), static_cast<float>(height),
                                      0.5f * static_cast<float>(depth)));
                        CHECK(space.inflated(1e-5f).contains(prop.bounds));
                        CHECK(prop.bounds.min.y == Approx(0.0f).margin(1e-6f)); // resting on the board
                        // It dresses the level: a good part of its width and height.
                        CHECK(prop.bounds.size().x > 0.2f * static_cast<float>(width));
                        CHECK(prop.bounds.size().y > 0.05f * static_cast<float>(height));
                        if (width == 0.85 && height == 0.17 && depth == 0.23) {
                            variants.insert(hashes(prop));
                            // The same parameters give the same prop.
                            CHECK(hashes(buildOk(kind, params)) == hashes(prop));
                        }
                    }
                }
            }
        }
        INFO(kind);
        CHECK(variants.size() == 4); // every seed its own variant
    }
    // A level of the collection's modules gets more than one box or stack.
    CHECK(buildOk("storage_box").parts.front().mesh.vertices.size() > 2 * 96);

    // Sized to a module's Level socket and placed by it, a prop stays in the level's free volume.
    const ProceduralPrefab module = buildOk("shelf", moduleParams(false));
    const Socket& level = socket(module, "Level_2");
    const glm::vec3 free = rv::geometry::socketSize(level);
    for (const std::string_view kind : {"storage_box", "magazine_stack", "bookends"}) {
        INFO(kind);
        const ProceduralPrefab prop =
            buildOk(kind, {{"width", free.x}, {"height", free.y}, {"depth", free.z}, {"seed", 31}});
        CHECK(level.bounds.transformed(level.transform)
                  .inflated(1e-5f)
                  .contains(prop.bounds.transformed(level.transform)));
    }
}
