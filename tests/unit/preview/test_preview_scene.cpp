#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

#include "support/temp_dir.hpp"
#include <rv/app/model_viewer.hpp>
#include <rv/assets/glb_writer.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/preview/asset_report.hpp>
#include <rv/preview/crt_fixture.hpp>
#include <rv/preview/preview_scene.hpp>
#include <rv/render/texture_memory.hpp>
#include <rv/scene/scene_geometry.hpp>

using Catch::Approx;
using rv::preview::PreviewSubject;
using rv::preview::PreviewTarget;

namespace {

const std::filesystem::path kAssetRoot = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

rv::assets::MaterialLibrary realMaterials() {
    auto library = rv::assets::MaterialLibrary::loadFromFile(kAssetRoot / "materials/base/materials.json");
    REQUIRE(library.has_value());
    return std::move(*library);
}

PreviewSubject load(std::string_view text) {
    auto target = rv::preview::parsePreviewTarget(text);
    REQUIRE(target.has_value());
    return rv::preview::loadPreviewSubject(*target, kAssetRoot);
}

/// The fixture written to `dir` and loaded as a subject.
PreviewSubject fixtureSubject(const rv::test::TempDir& dir) {
    const auto file = dir.file("crt_fixture.glb");
    REQUIRE(rv::assets::writeGlb(file, rv::preview::makeCrtFixture()).has_value());
    return load(rv::core::pathToUtf8(file));
}

} // namespace

TEST_CASE("preview/scene: every prefab kind looks as in the test room, with real materials on every part",
          "[preview][scene]") {
    const rv::assets::MaterialLibrary library = realMaterials();
    for (const std::string_view kind : rv::geometry::prefabKinds()) {
        INFO(kind);
        const auto materials = rv::preview::prefabRoleMaterials(kind);
        for (const auto& [role, name] : materials) {
            INFO(role << " -> " << name);
            CHECK(library.find(name) != nullptr);
        }
        // The default prefab of the kind: every part has a material, none falls back to magenta.
        const PreviewSubject subject = load(std::string("prefab:") + std::string(kind));
        REQUIRE(subject.prefab.has_value());
        const rv::scene::SceneDescription scene = rv::preview::makePreviewScene(subject);
        REQUIRE(scene.objects.size() == 1);
        const rv::scene::SceneGeometry geometry = rv::scene::buildSceneGeometry(scene, library);
        CHECK(geometry.fallbackMaterialCount() == 0);
        CHECK(geometry.placeholderCount() == 0);
    }
    // Screens show the color bars, labels and posters a cover.
    CHECK(rv::preview::prefabRoleTextures("crt").at("screen").pattern == rv::preview::kScreenPattern);
    CHECK(rv::preview::prefabRoleTextures("cartridge").at("label").pattern == "cover");
    CHECK(rv::preview::prefabRoleTextures("poster_frame").at("picture").pattern == "cover");
    CHECK(rv::preview::prefabRoleMaterials("crt").at("screen") == rv::preview::kScreenMaterialName);
    CHECK(rv::preview::prefabRoleMaterials("television").empty());
}

TEST_CASE("preview/scene: a model is shown at the origin in the studio of the model viewer", "[preview][scene]") {
    rv::test::TempDir dir;
    const PreviewSubject subject = fixtureSubject(dir);
    const rv::scene::SceneDescription scene = rv::preview::makePreviewScene(subject);
    REQUIRE(scene.objects.size() == 1);
    const rv::scene::SceneObject& object = scene.objects[0];
    CHECK(object.source == rv::scene::SceneObject::Source::Model);
    CHECK(object.modelPath == rv::core::pathToUtf8(subject.modelFile));
    CHECK_FALSE(object.collides);
    // The studio: the model viewer's lights and background.
    const rv::scene::SceneDescription viewer = rv::app::makeModelViewerScene(object.modelPath, subject.bounds);
    CHECK(scene.lights.size() == viewer.lights.size());
    CHECK(scene.environment.clearColor == viewer.environment.clearColor);

    // The loader serves the subject's model; the scene then draws it without a placeholder.
    const rv::scene::SceneGeometry geometry =
        rv::scene::buildSceneGeometry(scene, realMaterials(), rv::preview::previewModelLoader(subject));
    CHECK(geometry.placeholderCount() == 0);
    CHECK(geometry.modelObjectCount() == 1);

    // An empty preview has no object; a subject that could not be loaded shows the placeholder.
    const rv::scene::SceneDescription empty = rv::preview::makePreviewScene(PreviewSubject{});
    CHECK(empty.objects.empty());
    const PreviewSubject missing = load(rv::core::pathToUtf8(dir.file("missing.glb")));
    const rv::scene::SceneGeometry placeholder = rv::scene::buildSceneGeometry(
        rv::preview::makePreviewScene(missing), realMaterials(), rv::preview::previewModelLoader(missing));
    CHECK(placeholder.placeholderCount() == 1);
}

TEST_CASE("preview/scene: screen surfaces get a 4:3 test pattern", "[preview][scene]") {
    rv::test::TempDir dir;
    const PreviewSubject fixture = fixtureSubject(dir);
    const auto screens = rv::preview::screenSurfaces(fixture);
    REQUIRE(screens.size() == 1);
    CHECK(screens[0].socketId == "ScreenSurface");
    REQUIRE(screens[0].modelMeshes.size() == 1);
    CHECK(fixture.model->meshes[screens[0].modelMeshes[0]].name == "Screen");
    CHECK(static_cast<float>(screens[0].patternSize.x) / static_cast<float>(screens[0].patternSize.y) ==
          Approx(4.0f / 3.0f).epsilon(0.02));

    // The procedural CRT: its screen role shows the pattern already, at the size the scene plans for it.
    const PreviewSubject crt = load("prefab:crt");
    const auto crtScreens = rv::preview::screenSurfaces(crt);
    REQUIRE(crtScreens.size() == 1);
    CHECK(crtScreens[0].modelMeshes.empty());
    CHECK(crtScreens[0].patternSize.x > crtScreens[0].patternSize.y);

    // A prefab without a screen has none.
    CHECK(rv::preview::screenSurfaces(load("prefab:shelf")).empty());

    // The screen's size as placed in the model: 48 x 36 cm on the fixture, a hundredth of it when the model's root is
    // scaled down 100 times (a model exported in centimeters). The pattern keeps the aspect.
    CHECK(rv::core::nearlyEqual(screens[0].size, glm::vec3(0.48f, 0.36f, 0.0f)));
    rv::assets::GlbDescription centimeters = rv::preview::makeCrtFixture();
    centimeters.nodes[0].scale = glm::vec3(0.01f);
    const auto scaledFile = dir.file("crt_fixture_cm.glb");
    REQUIRE(rv::assets::writeGlb(scaledFile, centimeters).has_value());
    const auto scaled = rv::preview::screenSurfaces(load(rv::core::pathToUtf8(scaledFile)));
    REQUIRE(scaled.size() == 1);
    CHECK(rv::core::nearlyEqual(scaled[0].size, glm::vec3(0.0048f, 0.0036f, 0.0f), 1e-7f));
    CHECK(scaled[0].bounds == screens[0].bounds); // the rectangle in the socket's own space is the same
}

TEST_CASE("preview/scene: view modes pick the surface view and wireframe", "[preview][scene]") {
    using rv::preview::ViewMode;
    CHECK(rv::preview::surfaceViewOf(ViewMode::Lit) == rv::gl::SurfaceView::Lit);
    CHECK(rv::preview::surfaceViewOf(ViewMode::BaseColor) == rv::gl::SurfaceView::BaseColor);
    CHECK(rv::preview::surfaceViewOf(ViewMode::Normals) == rv::gl::SurfaceView::Normals);
    CHECK(rv::preview::surfaceViewOf(ViewMode::UvChecker) == rv::gl::SurfaceView::UvChecker);
    CHECK(rv::preview::surfaceViewOf(ViewMode::Wireframe) == rv::gl::SurfaceView::Normals);
    CHECK(rv::preview::wireframeOf(ViewMode::Wireframe));
    CHECK_FALSE(rv::preview::wireframeOf(ViewMode::Lit));
    std::set<std::string_view> names;
    for (const ViewMode mode : rv::preview::kViewModes) {
        names.insert(rv::preview::toString(mode));
    }
    CHECK(names.size() == 5);
}

TEST_CASE("preview/report: statistics of a model and of a prefab", "[preview][report]") {
    rv::test::TempDir dir;
    const rv::preview::AssetReport fixture = rv::preview::buildAssetReport(fixtureSubject(dir));
    CHECK(fixture.nodes == 6);
    CHECK(fixture.meshes == 3);
    CHECK(fixture.primitives == 2); // the body and the screen: the collision proxy is not drawn
    CHECK(fixture.triangles == 12 + 2);
    CHECK(fixture.vertices == 24 + 4);
    CHECK(fixture.materials == 2);
    CHECK(fixture.sockets == 4);
    REQUIRE(fixture.textures.size() == 1);
    CHECK(fixture.textures[0].size == glm::ivec2(32, 32));
    CHECK(fixture.textures[0].usage == "base color of Fixture_Plastic");
    CHECK(fixture.textures[0].gpuBytes == rv::gl::mipChainBytes(glm::ivec2(32), 6));
    CHECK(fixture.textureBytes == fixture.textures[0].gpuBytes);
    CHECK(fixture.size.x == Approx(0.71f));

    const rv::preview::AssetReport crt = rv::preview::buildAssetReport(load("prefab:crt"));
    CHECK(crt.meshes == 4);
    CHECK(crt.primitives == 4);
    CHECK(crt.materials == 4);
    CHECK(crt.triangles > 100);
    CHECK(crt.textures.empty());
    CHECK(crt.sockets == 4);
    CHECK(crt.slots.empty());

    // A collection module (I3 step 1b): its slots grouped by the volume an item fills, spine-out then face-out.
    const rv::preview::AssetReport hero = rv::preview::buildAssetReport(
        load(rv::core::pathToUtf8(kAssetRoot / "manifests/prefabs/furniture.shelf.hero_module.generic.json")));
    REQUIRE(hero.slots.size() == 2);
    CHECK(hero.slots[0].size.x == Approx(0.02f));
    CHECK(hero.slots[0].size.y == Approx(0.125f));
    CHECK(hero.slots[0].size.z == Approx(0.085f));
    CHECK(hero.slots[0].slots == 198);
    CHECK(hero.slots[0].levels == 6);
    CHECK(hero.slots[1].size.x == Approx(0.125f));
    CHECK(hero.slots[1].slots == 6);
    CHECK(hero.slots[1].levels == 1);
    CHECK(hero.sockets == 204 + 7); // the slots and the 7 Level anchors
    const rv::preview::AssetReport display = rv::preview::buildAssetReport(load("prefab:display_case"));
    REQUIRE(display.slots.size() == 1);
    CHECK(display.slots[0].slots == 16);
    CHECK(display.slots[0].levels == 2);

    const rv::preview::AssetReport empty = rv::preview::buildAssetReport(PreviewSubject{});
    CHECK(empty.triangles == 0);
    CHECK(empty.size == glm::vec3(0.0f));
}
