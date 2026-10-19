// A plan's static part as a scene (rv/world/scene_builder.hpp; I3 step 10a) on the golden plans of step 3: golden
// snapshots (tests/fixtures/world/scene: the digests of every golden plan's scene file and collection layout, the
// whole of one plan's, rewritten only with RV_UPDATE_GOLDEN=1), two runs alike; every fixture's prefab built with its
// parameters at its pivot and yaw, with a material for every role; the room shell sized to the plan; the dressing in
// its levels' free volumes; colliders on every fixture but the rug and on no collection item; the lights with the
// lighting profile's colours and intensities; the file read back as the scene description.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/canonical_json.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/sha256.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>

#include "plan_scene_support.hpp"
#include "support/temp_dir.hpp"

using rv::test::world::goldenPlanNames;
using rv::test::world::layoutOf;
using rv::test::world::PlanCase;
using rv::test::world::planCase;

namespace {

const std::filesystem::path kSceneGoldenDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "scene";
/// The plan whose scene file and collection layout are kept whole (a face-out section, the band and one spine-out
/// title, dressing of every kind).
constexpr std::string_view kWholeSnapshotPlan = "plan-57-seed-102938";

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN");
    return value != nullptr && std::string_view(value) == "1";
}

rv::world::SceneBuild sceneOf(const PlanCase& entry) {
    auto build = rv::world::toSceneFile(entry.plan, entry.room, entry.data);
    INFO((build ? std::string("built") : build.error().toString()));
    REQUIRE(build.has_value());
    return std::move(*build);
}

rv::scene::SceneDescription descriptionOf(const PlanCase& entry) {
    auto scene = rv::world::toSceneDescription(entry.plan, entry.room, entry.data);
    INFO((scene ? std::string("parsed") : scene.error().toString()));
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

std::string canonical(const nlohmann::json& value) {
    auto text = rv::core::writeCanonicalJson(value);
    INFO((text ? std::string("written") : text.error().toString()));
    REQUIRE(text.has_value());
    return std::move(*text);
}

/// Compares `text` with the golden file `name` (or rewrites it with RV_UPDATE_GOLDEN=1).
void checkGolden(const std::string& name, const std::string& text) {
    const std::filesystem::path golden = kSceneGoldenDir / name;
    if (updatingGoldens()) {
        REQUIRE(rv::core::writeTextFileAtomic(golden, text).has_value());
        WARN("rewrote " << rv::core::pathToUtf8(golden));
        return;
    }
    auto expected = rv::core::readTextFile(golden);
    INFO(name << " (RV_UPDATE_GOLDEN=1 rewrites it)");
    REQUIRE(expected.has_value());
    CHECK(*expected == text);
}

const rv::scene::SceneObject& objectOf(const rv::scene::SceneDescription& scene, std::string_view id) {
    const rv::scene::SceneObject* object = scene.findObject(id);
    INFO("object " << id);
    REQUIRE(object != nullptr);
    return *object;
}

float maxDifference(const glm::mat4& a, const glm::mat4& b) {
    float difference = 0.0f;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            difference = std::max(difference, std::abs(a[c][r] - b[c][r]));
        }
    }
    return difference;
}

std::int64_t mm(float meters) {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(meters) * 1000.0));
}

} // namespace

TEST_CASE("world/plan_scene: golden snapshots of every golden plan's scene and collection, two runs alike",
          "[world][plan_scene]") {
    nlohmann::json digests = nlohmann::json::object();
    for (const std::string& name : goldenPlanNames()) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const rv::world::SceneBuild build = sceneOf(entry);
        const rv::world::CollectionLayout layout = layoutOf(entry);
        const std::string sceneText = rv::world::sceneFileText(build.sceneFile);
        const std::string collectionText = canonical(rv::world::collectionLayoutToJson(layout));
        // A second run writes the same texts.
        CHECK(rv::world::sceneFileText(sceneOf(entry).sceneFile) == sceneText);
        CHECK(canonical(rv::world::collectionLayoutToJson(layoutOf(entry))) == collectionText);
        digests[name] = nlohmann::json{{"scene", rv::core::sha256Hex(sceneText)},
                                       {"collection", rv::core::sha256Hex(collectionText)},
                                       {"objects", build.sceneFile["objects"].size()},
                                       {"lights", build.sceneFile["lights"].size()},
                                       {"undressed", build.undressed.size()},
                                       {"items", layout.items.size()},
                                       {"clusters", layout.clusters.size()},
                                       {"occluders", layout.occluders.size()}};
        if (name == kWholeSnapshotPlan) {
            checkGolden(name + ".scene.json", sceneText);
            checkGolden(name + ".collection.json", collectionText);
        }
    }
    checkGolden("digests.json", canonical(digests));
}

TEST_CASE("world/plan_scene: every fixture's prefab built with its parameters, at its pivot and yaw",
          "[world][plan_scene]") {
    for (const std::string& name : goldenPlanNames()) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const rv::scene::SceneDescription scene = descriptionOf(entry);
        CHECK(scene.name == entry.room.toString());
        std::set<std::string> ids;
        for (const rv::scene::SceneObject& object : scene.objects) {
            CHECK(ids.insert(object.id).second);
            // The collection's items are never scene objects (ADR-0011 §1).
            CHECK(object.kind != entry.data.mediaFamily.item.kind);
        }
        std::size_t fixtures = 0;
        for (const rv::world::Fixture& fixture : entry.plan.fixtures) {
            INFO(fixture.id.toString());
            ++fixtures;
            const rv::scene::SceneObject& object = objectOf(scene, fixture.id.localText());
            const rv::world::ScenePrefab* prefab = entry.data.prefabs.find(fixture.prefab);
            REQUIRE(prefab != nullptr);
            REQUIRE(object.source == rv::scene::SceneObject::Source::Prefab);
            CHECK(object.kind == fixture.prefab.kind);
            CHECK(object.params == prefab->params);
            auto built = rv::geometry::buildPrefab(object.kind, object.params);
            REQUIRE(built.has_value());
            // The prefab the plan measured: its bounds in whole millimetres.
            const glm::vec3 size = built->bounds.size();
            CHECK(mm(size.x) == fixture.widthMm);
            CHECK(mm(size.y) == fixture.heightMm);
            CHECK(mm(size.z) == fixture.depthMm);
            const glm::mat4 expected = glm::rotate(
                glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(fixture.xMm), static_cast<float>(fixture.yMm),
                                                          static_cast<float>(fixture.zMm)) /
                                                    1000.0f),
                glm::radians(static_cast<float>(fixture.yawCdeg) / 100.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            CHECK(maxDifference(object.transform, expected) < 1e-5f);
            // A material for every role it draws.
            for (const rv::geometry::PrefabPart& part : built->parts) {
                INFO(part.materialRole);
                CHECK_FALSE(object.materialNameForRole(part.materialRole).empty());
            }
            // Colliders on every fixture but the rug, which is walked over.
            const bool rug = object.kind == "rug";
            CHECK(object.collides == !rug);
            if (!rug) {
                REQUIRE_FALSE(built->colliders.empty());
                rv::core::Aabb covered;
                for (const rv::geometry::PrefabCollider& collider : built->colliders) {
                    covered.expand(collider.box);
                }
                CHECK(covered.contains(built->bounds));
            }
        }
        // The shell, the fixtures and every dressed span but those left bare.
        std::size_t dressed = 0;
        for (const rv::world::DressedLevel& span : entry.plan.dressing) {
            dressed += span.level.fixture().room() == entry.room ? 1 : 0;
        }
        const rv::world::SceneBuild build = sceneOf(entry);
        CHECK(scene.objects.size() == 1 + fixtures + dressed - build.undressed.size());
    }
}

TEST_CASE("world/plan_scene: the room shell is sized to the plan's room", "[world][plan_scene]") {
    for (const std::string& name : {"plan-890-seed-102938", "plan-8000-seed-102938"}) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const rv::world::Room& room = entry.plan.rooms.front();
        const rv::scene::SceneDescription scene = descriptionOf(entry);
        const rv::scene::SceneObject& shell = objectOf(scene, "room");
        CHECK(shell.kind == "room_shell");
        CHECK(shell.params["size"][0].get<double>() == static_cast<double>(room.maxXMm - room.minXMm) / 1000.0);
        CHECK(shell.params["size"][1].get<double>() == static_cast<double>(room.maxZMm - room.minZMm) / 1000.0);
        CHECK(shell.params["wallHeight"].get<double>() == static_cast<double>(room.heightMm) / 1000.0);
        CHECK(shell.params["doorway"]["wall"] == "south");
        const glm::vec3 centre(static_cast<float>(room.minXMm + room.maxXMm) / 2000.0f, 0.0f,
                               static_cast<float>(room.minZMm + room.maxZMm) / 2000.0f);
        CHECK(glm::length(glm::vec3(shell.transform[3]) - centre) < 1e-6f);
        auto built = rv::geometry::buildPrefab(shell.kind, shell.params);
        REQUIRE(built.has_value());
        // Every fixture stands inside the room's interior (its walls' inner faces).
        for (const rv::world::Fixture& fixture : entry.plan.fixtures) {
            CHECK(fixture.xMm > room.minXMm);
            CHECK(fixture.xMm < room.maxXMm);
            CHECK(fixture.zMm > room.minZMm);
            CHECK(fixture.zMm < room.maxZMm);
        }
        // The camera inside the doorway, looking north, inside the room.
        CHECK(scene.camera.yawDeg == 0.0f);
        CHECK(scene.camera.position.z < static_cast<float>(room.maxZMm) / 1000.0f);
        CHECK(scene.camera.position.z > static_cast<float>(room.maxZMm) / 1000.0f - 1.5f);
    }
    // The grown room's shell stands south of the origin, its north wall where the base room's is.
    const rv::world::Room& grown = planCase("plan-8000-seed-102938").plan.rooms.front();
    CHECK(grown.growthSteps == 3);
    CHECK(grown.minZMm == -5000);
    CHECK(grown.maxZMm == 11000);
}

TEST_CASE("world/plan_scene: the dressing stands in its levels' free volumes; display cases' levels stay bare",
          "[world][plan_scene]") {
    for (const std::string& name : {"plan-0-seed-102938", "plan-15-seed-7", "plan-50-seed-102938",
                                    "plan-57-seed-102938", "plan-890-seed-102938"}) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const rv::scene::SceneDescription scene = descriptionOf(entry);
        const rv::world::SceneBuild build = sceneOf(entry);
        std::set<std::pair<rv::world::LevelId, std::uint32_t>> bare;
        for (const rv::world::UndressedSpan& span : build.undressed) {
            bare.emplace(span.level, span.firstSlot);
            // Only the display case's leaning levels are too shallow for the dressing's props.
            CHECK(span.level.fixture().socket().rfind("dc", 0) == 0);
            CHECK_THAT(span.reason, Catch::Matchers::ContainsSubstring("outside what a"));
        }
        for (const rv::world::DressedLevel& dressed : entry.plan.dressing) {
            if (bare.contains({dressed.level, dressed.firstSlot})) {
                continue;
            }
            std::string id = "dress:" + dressed.level.localText();
            if (dressed.firstSlot != 0) {
                id += "." + std::to_string(dressed.firstSlot);
            }
            INFO(id);
            const rv::scene::SceneObject& object = objectOf(scene, id);
            if (dressed.level.fixture().socket().rfind("dc", 0) == 0 && object.kind == "poster_frame") {
                // A display case's level too thin for the prop: its riser card (I3 step 16).
                CHECK_FALSE(object.castsShadows);
                CHECK(rv::geometry::buildPrefab(object.kind, object.params).has_value());
                continue;
            }
            CHECK(object.kind == dressed.prop.kind);
            CHECK(object.castsShadows);
            auto built = rv::geometry::buildPrefab(object.kind, object.params);
            REQUIRE(built.has_value());
            // The prop inside its level's free volume (the Level anchor's bounds) in the fixture, within 1 mm.
            const rv::world::Fixture* fixture = nullptr;
            for (const rv::world::Fixture& candidate : entry.plan.fixtures) {
                if (candidate.id == dressed.level.fixture()) {
                    fixture = &candidate;
                }
            }
            REQUIRE(fixture != nullptr);
            const rv::scene::SceneObject& fixtureObject = objectOf(scene, fixture->id.localText());
            auto fixturePrefab = rv::geometry::buildPrefab(fixtureObject.kind, fixtureObject.params);
            REQUIRE(fixturePrefab.has_value());
            const rv::geometry::Socket* level = rv::geometry::findSocket(
                fixturePrefab->sockets, "Level_" + std::to_string(dressed.level.level()));
            REQUIRE(level != nullptr);
            const glm::mat4 toLevel = glm::inverse(fixtureObject.transform * level->transform);
            for (const glm::vec3& corner : rv::test::world::corners(object.transform, built->bounds)) {
                const glm::vec3 local = glm::vec3(toLevel * glm::vec4(corner, 1.0f));
                CHECK(level->bounds.inflated(0.001f).contains(local));
            }
        }
    }
    // The empty room dresses every level of the modules and the wall shelf, and the display cases' levels, too thin for
    // a prop, with riser cards (I3 step 16): no level stands bare.
    const rv::world::SceneBuild empty = sceneOf(planCase("plan-0-seed-102938"));
    CHECK(empty.undressed.empty());
    std::size_t cards = 0;
    for (const auto& object : empty.sceneFile["objects"]) {
        if (object["id"].get<std::string>().rfind("dress:fx:dc0", 0) == 0) {
            CHECK(object["prefab"] == "poster_frame");
            CHECK(object["materials"]["picture"] == "MAT_Label_Print");
            CHECK(object["textures"]["picture"]["pattern"] == "cover");
            ++cards;
        }
    }
    CHECK(cards == 4);
}

TEST_CASE("world/plan_scene: the lights with the lighting profile's colours and intensities",
          "[world][plan_scene]") {
    // The shipped default profile (assets/world/lighting/evening.json), which the scene data reads.
    const auto loaded = rv::world::loadLightingProfile(rv::test::world::sourceAssetRoot(), "evening");
    REQUIRE(loaded.has_value());
    const rv::world::LightingProfile& profile = *loaded;
    REQUIRE_FALSE(profile.accents.empty());
    for (const std::string& name : {"plan-0-seed-102938", "plan-890-seed-102938", "plan-8000-seed-102938"}) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const rv::scene::SceneDescription scene = descriptionOf(entry);
        // The plan's lights, then the profile's accents while the scene's limit leaves room for them: one on its hero
        // corner part, or one on each display fixture of its kind (in the plan's fixture order).
        std::vector<std::pair<std::string, std::string>> accents; // name, colour
        for (const rv::world::AccentLight& accent : profile.accents) {
            if (accent.on == rv::world::AccentAnchor::HeroCorner) {
                accents.emplace_back("accent:" + accent.name, accent.color);
                continue;
            }
            for (const rv::world::Fixture& fixture : entry.plan.fixtures) {
                if (fixture.role == rv::world::FixtureRole::Display && fixture.prefab.kind == accent.anchor) {
                    accents.emplace_back("accent:" + accent.name + ":" + fixture.id.socket(), accent.color);
                }
            }
        }
        CHECK(std::any_of(accents.begin(), accents.end(),
                          [](const auto& accent) { return accent.first == "accent:case_light:dc01"; }));
        REQUIRE(scene.lights.size() >= entry.plan.lights.size());
        CHECK(scene.lights.size() ==
              std::min<std::size_t>(rv::world::kMaxSceneLights, entry.plan.lights.size() + accents.size()));
        for (std::size_t i = entry.plan.lights.size(); i < scene.lights.size(); ++i) {
            const auto& [accentName, accentColor] = accents[i - entry.plan.lights.size()];
            CHECK(scene.lights[i].name == accentName);
            CHECK(scene.lights[i].color == *rv::core::hexToLinearRgb(accentColor));
            CHECK_FALSE(scene.lights[i].castsShadows);
        }
        std::size_t shadowed = 0;
        for (std::size_t i = 0; i < entry.plan.lights.size(); ++i) {
            const rv::world::PlanLight& planned = entry.plan.lights[i];
            const rv::scene::Light& light = scene.lights[i];
            INFO(planned.id.toString());
            const rv::world::LightStyle& style = profile.style(planned.role);
            CHECK(light.name == planned.id.localText());
            CHECK(light.color == *rv::core::hexToLinearRgb(style.color));
            CHECK(light.intensity == static_cast<float>(style.intensity));
            CHECK(light.range == static_cast<float>(style.range));
            const glm::vec3 position = glm::vec3(static_cast<float>(planned.xMm), static_cast<float>(planned.yMm),
                                                 static_cast<float>(planned.zMm)) /
                                       1000.0f;
            CHECK(glm::length(light.position - position) < 1e-6f);
            if (rv::world::lightKind(planned.role) == rv::world::LightKind::Point) {
                CHECK(light.type == rv::scene::LightType::Point);
            } else {
                CHECK(light.type == rv::scene::LightType::Spot);
                const glm::vec3 target = glm::vec3(static_cast<float>(planned.targetXMm),
                                                   static_cast<float>(planned.targetYMm),
                                                   static_cast<float>(planned.targetZMm)) /
                                         1000.0f;
                CHECK(glm::length(light.direction - glm::normalize(target - position)) < 1e-5f);
                // The cone covers the span at the target, at least the role's minimum, the inner one inside it.
                const float halfSpan = static_cast<float>(planned.spanMm) / 2000.0f;
                const float subtended = glm::degrees(std::atan2(halfSpan, glm::length(target - position)));
                CHECK(light.outerConeDeg >= subtended);
                CHECK(light.outerConeDeg >= static_cast<float>(style.minOuterConeDeg) - 0.01f);
                CHECK(light.outerConeDeg <= static_cast<float>(rv::world::kMaxOuterConeDeg));
                CHECK(light.innerConeDeg <= light.outerConeDeg);
            }
            CHECK(light.castsShadows == (planned.role == rv::world::LightRole::Key));
            shadowed += light.castsShadows ? 1 : 0;
        }
        CHECK(shadowed == 1);
        // The profile's environment.
        CHECK(scene.environment.clearColor == *rv::core::hexToLinearRgb(profile.clearColor));
        CHECK(scene.environment.exposure == static_cast<float>(profile.exposure));
        CHECK(scene.environment.ambientIntensity == static_cast<float>(profile.ambientIntensity));
    }
}

TEST_CASE("world/plan_scene: the scene file reads back as the scene description, from a file too",
          "[world][plan_scene]") {
    const rv::test::TempDir dir("rv-world-plan-scene");
    const PlanCase& entry = planCase("plan-890-seed-102938");
    const rv::world::SceneBuild build = sceneOf(entry);
    const std::filesystem::path file = dir.file("room.json");
    REQUIRE(rv::core::writeTextFileAtomic(file, rv::world::sceneFileText(build.sceneFile)).has_value());
    auto loaded = rv::scene::loadSceneDescription(file);
    INFO((loaded ? std::string("loaded") : loaded.error().toString()));
    REQUIRE(loaded.has_value());
    const rv::scene::SceneDescription scene = descriptionOf(entry);
    REQUIRE(loaded->objects.size() == scene.objects.size());
    for (std::size_t i = 0; i < scene.objects.size(); ++i) {
        CHECK(loaded->objects[i].id == scene.objects[i].id);
        CHECK(loaded->objects[i].params == scene.objects[i].params);
        CHECK(loaded->objects[i].transform == scene.objects[i].transform);
    }
    CHECK(loaded->lights.size() == scene.lights.size());
    // The sign shows the platform's text; the CRT its idle colour bars.
    const rv::scene::SceneObject& sign = *loaded->findObject("fx:hero.sign");
    CHECK(sign.texturesByRole.at("picture").pattern == "sign");
    CHECK(sign.texturesByRole.at("picture").title == entry.data.platform.signText);
    CHECK(loaded->findObject("fx:hero.crt")->texturesByRole.at("screen").pattern == "color_bars");
    CHECK(loaded->findObject("fx:hero.crt")->materialNameForRole("screen") == "MAT_Screen_CRT_Room");
}

TEST_CASE("world/plan_scene: the theme dresses every role of every prefab kind, the collection's item included",
          "[world][plan_scene]") {
    for (const std::string_view kind : rv::geometry::prefabKinds()) {
        INFO(kind);
        const auto materials = rv::world::themeMaterials(kind);
        REQUIRE_FALSE(materials.empty());
        auto built = rv::geometry::buildPrefab(kind, nlohmann::json::object());
        REQUIRE(built.has_value());
        for (const rv::geometry::PrefabPart& part : built->parts) {
            INFO(part.materialRole);
            CHECK(materials.contains(part.materialRole));
        }
    }
    CHECK(rv::world::themeMaterials("cartridge").contains("spine"));
    CHECK(rv::world::themeMaterials("no_such_kind").empty());
}

TEST_CASE("world/plan_scene: refused inputs", "[world][plan_scene]") {
    const PlanCase& entry = planCase("plan-50-seed-102938");
    // Another room.
    auto other = rv::world::toSceneFile(entry.plan, *rv::world::RoomId::parse("room:nes"), entry.data);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().code == rv::ErrorCode::NotFound);
    // A prefab the data lacks.
    rv::world::SceneData missing = entry.data;
    missing.prefabs.prefabs.clear();
    auto lacking = rv::world::toSceneFile(entry.plan, entry.room, missing);
    REQUIRE_FALSE(lacking.has_value());
    CHECK(lacking.error().code == rv::ErrorCode::NotFound);
    // A manifest that builds another kind than the plan names.
    auto families = rv::assets::loadPrefabFamilies(rv::test::world::sourceAssetRoot());
    REQUIRE(families.has_value());
    const std::vector<rv::world::PrefabRef> wrong{{"furniture.armchair.generic", "shelf"}};
    auto refused = rv::world::loadScenePrefabs(rv::test::world::sourceAssetRoot(), *families, wrong);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::ValidationFailed);
    // Seventeen lights.
    rv::world::WorldPlan crowded = entry.plan;
    while (crowded.lights.size() <= rv::world::kMaxSceneLights) {
        rv::world::PlanLight light = crowded.lights.front();
        light.id = *rv::world::FixtureId::make(entry.room, "room", "extra" + std::to_string(crowded.lights.size()));
        light.role = rv::world::LightRole::Pendant;
        light.targetXMm = light.xMm;
        light.targetYMm = light.yMm;
        light.targetZMm = light.zMm;
        light.spanMm = 0;
        crowded.lights.push_back(light);
    }
    auto tooMany = rv::world::toSceneFile(crowded, entry.room, entry.data);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().code == rv::ErrorCode::ValidationFailed);
}
