// Cross-module integration checks (GPU-free): the reference scene files, the procedural geometry kit, the material
// library and the prefab manifests were written by independent work streams against shared contracts. These tests
// build the real geometry and resolve the real materials so contract drift fails loudly.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <rv/assets/material_library.hpp>
#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/scene_description.hpp>

namespace fs = std::filesystem;
using rv::scene::SceneObject;

namespace {

fs::path assetDir() {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
}

std::vector<fs::path> filesIn(const fs::path& directory, std::string_view extension) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

glm::vec3 translationOf(const glm::mat4& m) {
    return glm::vec3(m[3]);
}

constexpr std::array<std::string_view, 3> kPatterns = {"color_bars", "checker", "cover"};

/// Checks that an object's material roles, textures and geometry resolve against the real modules.
void checkObject(const SceneObject& object, const rv::assets::MaterialLibrary& library) {
    INFO("object " << object.id);
    const bool hasInline = object.inlineMaterial.is_object();
    const auto requireMaterial = [&](std::string_view role, std::string_view name) {
        INFO("role " << role << " material " << name);
        if (name.empty()) {
            CHECK(hasInline); // otherwise the renderer would silently fall back to magenta
        } else {
            CHECK(library.find(name) != nullptr);
        }
    };

    switch (object.source) {
    case SceneObject::Source::Prefab: {
        const auto prefab = rv::geometry::buildPrefab(object.kind, object.params);
        REQUIRE(prefab.has_value());
        for (const auto& part : prefab->parts) {
            requireMaterial(part.materialRole, object.materialNameForRole(part.materialRole));
        }
        for (const auto& [role, name] : object.materialsByRole) {
            INFO("materials." << role);
            CHECK(rv::geometry::findPart(*prefab, role) != nullptr);
        }
        for (const auto& [role, texture] : object.texturesByRole) {
            INFO("textures." << role);
            CHECK(rv::geometry::findPart(*prefab, role) != nullptr);
            if (texture.kind == rv::scene::TextureSource::Kind::Pattern) {
                CHECK(std::find(kPatterns.begin(), kPatterns.end(), texture.pattern) != kPatterns.end());
            } else {
                CHECK(fs::is_regular_file(assetDir() / rv::core::pathFromUtf8(texture.path)));
            }
        }
        break;
    }
    case SceneObject::Source::Primitive: {
        const auto mesh = rv::geometry::buildPrimitive(object.kind, object.params);
        REQUIRE(mesh.has_value());
        requireMaterial(object.kind, object.material);
        break;
    }
    case SceneObject::Source::Model:
        CHECK(fs::is_regular_file(assetDir() / rv::core::pathFromUtf8(object.modelPath)));
        break;
    }
}

} // namespace

TEST_CASE("integration/reference_scenes: every object builds and resolves its materials", "[integration][scene]") {
    const auto library = rv::assets::MaterialLibrary::loadFromFile(assetDir() / "materials/base/materials.json");
    REQUIRE(library.has_value());

    const auto files = filesIn(assetDir() / "style_tests/reference_scenes", ".json");
    REQUIRE(files.size() >= 3);
    for (const fs::path& file : files) {
        INFO("scene " << rv::core::pathToUtf8(file.filename()));
        const auto scene = rv::scene::loadSceneDescription(file);
        REQUIRE(scene.has_value());
        for (const SceneObject& object : scene->objects) {
            checkObject(object, *library);
        }
    }
}

TEST_CASE("integration/m0_testroom: cartridges sit exactly in shelf slots", "[integration][scene]") {
    const auto scene =
        rv::scene::loadSceneDescription(assetDir() / "style_tests/reference_scenes/m0_testroom.json");
    REQUIRE(scene.has_value());

    const SceneObject* shelf = nullptr;
    for (const SceneObject& object : scene->objects) {
        if (object.source == SceneObject::Source::Prefab && object.kind == "shelf") {
            REQUIRE(shelf == nullptr); // the M0 room has a single shelf
            shelf = &object;
        }
    }
    REQUIRE(shelf != nullptr);
    const auto shelfPrefab = rv::geometry::buildPrefab("shelf", shelf->params);
    REQUIRE(shelfPrefab.has_value());

    std::set<std::string> usedSlots;
    int cartridges = 0;
    for (const SceneObject& object : scene->objects) {
        if (object.source != SceneObject::Source::Prefab || object.kind != "cartridge") {
            continue;
        }
        ++cartridges;
        INFO("cartridge " << object.id);
        const auto cartridge = rv::geometry::buildPrefab("cartridge", object.params);
        REQUIRE(cartridge.has_value());
        const float halfHeight = 0.5f * cartridge->bounds.size().y;
        const glm::vec3 position = translationOf(object.transform);

        std::optional<std::string> matched;
        for (const auto& socket : shelfPrefab->sockets) {
            if (socket.kind != rv::geometry::SocketKind::ShelfSlot) {
                continue;
            }
            const glm::mat4 slotWorld = shelf->transform * socket.transform;
            const glm::vec3 expected = glm::vec3(slotWorld * glm::vec4(0.0f, halfHeight, 0.0f, 1.0f));
            if (glm::length(expected - position) < 1e-4f) {
                matched = socket.id;
                // Same orientation as the slot (front facing the shelf's +Z).
                CHECK(rv::core::nearlyEqual(glm::mat3(object.transform)[0], glm::mat3(slotWorld)[0], 1e-4f));
                CHECK(rv::core::nearlyEqual(glm::mat3(object.transform)[2], glm::mat3(slotWorld)[2], 1e-4f));
                // The item fits the slot volume.
                const glm::vec3 slotSize = socket.bounds.size();
                const glm::vec3 itemSize = cartridge->bounds.size();
                CHECK(itemSize.x <= slotSize.x + 1e-5f);
                CHECK(itemSize.y <= slotSize.y + 1e-5f);
                CHECK(itemSize.z <= slotSize.z + 1e-5f);
                break;
            }
        }
        REQUIRE(matched.has_value());
        CHECK(usedSlots.insert(*matched).second); // one cartridge per slot
    }
    CHECK(cartridges == 24);
}

TEST_CASE("integration/m0_testroom: the lamp light sits at the lamp's LightSource socket", "[integration][scene]") {
    const auto scene =
        rv::scene::loadSceneDescription(assetDir() / "style_tests/reference_scenes/m0_testroom.json");
    REQUIRE(scene.has_value());

    const SceneObject* lamp = nullptr;
    for (const SceneObject& object : scene->objects) {
        if (object.source == SceneObject::Source::Prefab && object.kind == "floor_lamp") {
            lamp = &object;
        }
    }
    REQUIRE(lamp != nullptr);
    const auto prefab = rv::geometry::buildPrefab("floor_lamp", lamp->params);
    REQUIRE(prefab.has_value());
    const auto* source = rv::geometry::findSocket(prefab->sockets, "LightSource");
    REQUIRE(source != nullptr);
    const glm::vec3 bulb = translationOf(lamp->transform * source->transform);

    float nearest = std::numeric_limits<float>::max();
    for (const auto& light : scene->lights) {
        if (light.type == rv::scene::LightType::Point) {
            nearest = std::min(nearest, glm::length(light.position - bulb));
        }
    }
    CHECK(nearest < 0.02f);
}

TEST_CASE("integration/manifests: procedural prefab manifests name real sockets", "[integration][assets]") {
    const auto files = filesIn(assetDir() / "manifests/prefabs", ".json");
    REQUIRE_FALSE(files.empty());
    for (const fs::path& file : files) {
        INFO("manifest " << rv::core::pathToUtf8(file.filename()));
        const auto manifest = rv::assets::loadPrefabManifest(file);
        REQUIRE(manifest.has_value());
        REQUIRE(manifest->runtime.generator.has_value());
        const auto prefab = rv::geometry::buildPrefab(*manifest->runtime.generator, manifest->runtime.params);
        REQUIRE(prefab.has_value());
        const auto issues = rv::assets::validatePrefabManifest(
            *manifest, nullptr, std::span<const rv::geometry::Socket>(prefab->sockets));
        for (const auto& issue : issues) {
            if (issue.severity == rv::assets::ValidationIssue::Severity::Error) {
                FAIL_CHECK(issue.message);
            }
        }
    }
}
