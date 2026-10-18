// The collection's fixtures as the world generator reads them (ADR-0010 §3, §5; I3 step 1b): the shipped manifests
// built and their slot sockets counted, the figures D, B, Sh and S recorded in docs/i3/step-1b.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>

using Catch::Approx;
using rv::geometry::SlotFacing;
using rv::geometry::SlotLevel;

namespace {

const std::filesystem::path kAssetRoot = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

/// The prefab of a shipped manifest, built with its parameters.
rv::geometry::ProceduralPrefab shipped(std::string_view id) {
    auto manifest = rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(kAssetRoot, id));
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->runtime.generator.has_value());
    auto prefab = rv::geometry::buildPrefab(*manifest->runtime.generator, manifest->runtime.params);
    REQUIRE(prefab.has_value());
    if (manifest->runtime.scaleMeters) {
        const glm::vec3 size = prefab->bounds.size();
        CHECK(size.x == Approx(manifest->runtime.scaleMeters->x));
        CHECK(size.y == Approx(manifest->runtime.scaleMeters->y));
        CHECK(size.z == Approx(manifest->runtime.scaleMeters->z));
    }
    return std::move(*prefab);
}

/// Slots of the levels whose slot volume is `slot`.
int slotsOf(const std::vector<SlotLevel>& levels, const glm::vec3& slot) {
    int count = 0;
    for (const SlotLevel& level : levels) {
        if (std::fabs(level.slotSize.x - slot.x) < 1e-5f && std::fabs(level.slotSize.y - slot.y) < 1e-5f &&
            std::fabs(level.slotSize.z - slot.z) < 1e-5f) {
            count += level.count;
        }
    }
    return count;
}

/// The cartridge of the media family: the item every slot is sized for.
constexpr glm::vec3 kCartridge{0.125f, 0.085f, 0.020f};

} // namespace

TEST_CASE("assets/collection: the fixtures hold D, B, Sh and S slots", "[assets][collection]") {
    const glm::vec3 cartridge = shipped("media.cartridge.home16.generic").bounds.size();
    CHECK(cartridge.x == Approx(kCartridge.x));
    CHECK(cartridge.y == Approx(kCartridge.y));
    CHECK(cartridge.z == Approx(kCartridge.z));
    const glm::vec3 faceOut = rv::geometry::slotSizeFor(SlotFacing::FaceOut, kCartridge);
    const glm::vec3 spineOut = rv::geometry::slotSizeFor(SlotFacing::SpineOut, kCartridge);
    CHECK(spineOut == glm::vec3(0.02f, 0.125f, 0.085f)); // the 2.0 x 12.5 x 8.5 cm spine-out slot

    // S: the side-wall module (the generic of the family furniture.shelf), 7 spine-out levels of 33.
    const auto side = rv::geometry::slotLevels(shipped("furniture.shelf.side_module.generic").sockets);
    REQUIRE(side.size() == 7);
    const int S = slotsOf(side, spineOut);
    CHECK(S == 231);
    CHECK(slotsOf(side, faceOut) == 0);

    // Sh and the hero band: 6 spine-out levels of 33 and one face-out level of 6 at eye height.
    const auto hero = rv::geometry::slotLevels(shipped("furniture.shelf.hero_module.generic").sockets);
    REQUIRE(hero.size() == 7);
    const int Sh = slotsOf(hero, spineOut);
    const int bandPerModule = slotsOf(hero, faceOut);
    CHECK(Sh == 198);
    CHECK(bandPerModule == 6);
    CHECK(hero[5].count == 6);
    CHECK(hero[5].height == Approx(1.375f)); // the face-out cartridges' centres at 1.42 m
    const int B = 2 * bandPerModule;         // the template's two hero-wall modules
    CHECK(B == 12);

    // D: the featured wall shelf and two display cases, all face-out.
    const auto wall = rv::geometry::slotLevels(shipped("furniture.wall_shelf.featured.generic").sockets);
    const auto display = rv::geometry::slotLevels(shipped("furniture.display_case.generic").sockets);
    const int featured = slotsOf(wall, faceOut);
    const int perCase = slotsOf(display, faceOut);
    CHECK(featured == 12);
    CHECK(perCase == 16);
    const int D = featured + 2 * perCase;
    CHECK(D == 44);

    // ADR-0010 §3, §5 with these figures and the reserve of 0.15: the 9 x 10 m room (14 side-wall sockets) holds
    // about 3,200 titles, 16 m deep (26) about 5,600; 890 titles need 3 side-wall modules, 2,000 need 8.
    const auto capacity = [&](int sideSockets) {
        return static_cast<int>(std::floor((D + B + 2 * Sh + sideSockets * S) / 1.15));
    };
    CHECK(capacity(14) == 3205);
    CHECK(capacity(26) == 5615);
    const auto sideModules = [&](int titles) {
        const int faceOutTitles = std::min(titles, D + B);
        const int reserve = static_cast<int>(std::ceil(titles * 0.15));
        return std::max(0, static_cast<int>(std::ceil(static_cast<double>(titles - faceOutTitles + reserve - 2 * Sh) /
                                                      static_cast<double>(S))));
    };
    CHECK(sideModules(890) == 3);
    CHECK(sideModules(2000) == 8);
    CHECK(sideModules(56) == 0);
    CHECK(sideModules(50) == 0);
}

TEST_CASE("assets/collection: the shelf dressing kinds share the parameters a level gives them",
          "[assets][collection]") {
    const nlohmann::json level{{"width", 0.79}, {"height", 0.175}, {"depth", 0.232}, {"seed", 12345}};
    for (const std::string_view id :
         {"decor.storage_box.generic", "decor.magazine_stack.generic", "decor.bookends.generic"}) {
        INFO(id);
        auto manifest = rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(kAssetRoot, id));
        REQUIRE(manifest.has_value());
        nlohmann::json params = manifest->runtime.params;
        params.update(level);
        auto prefab = rv::geometry::buildPrefab(*manifest->runtime.generator, params);
        REQUIRE(prefab.has_value());
        CHECK(prefab->bounds.size().x <= 0.79f + 1e-5f);
        CHECK(prefab->bounds.size().y <= 0.175f + 1e-5f);
        CHECK(prefab->bounds.size().z <= 0.232f + 1e-5f);
    }
}
