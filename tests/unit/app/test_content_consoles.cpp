// I11 step 7: the console rooms' hero consoles and the user's models in their place (app::consoleSites,
// consoleModelObject, consoleModelLocal, consoleModelSocket, contentModelPath).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/content_consoles.hpp>
#include <rv/assets/gltf_loader.hpp>

#include "support/fixture_console.hpp"
#include "support/temp_dir.hpp"

using Catch::Approx;

namespace {

rv::scene::SceneObject object(std::string id, rv::scene::SceneObject::Source source, const glm::mat4& transform) {
    rv::scene::SceneObject made;
    made.id = std::move(id);
    made.source = source;
    made.kind = source == rv::scene::SceneObject::Source::Prefab ? "console_cartridge" : "";
    made.transform = transform;
    return made;
}

} // namespace

TEST_CASE("app/content_consoles: the hero consoles of a scene", "[app][content_consoles]") {
    using Source = rv::scene::SceneObject::Source;
    rv::scene::SceneDescription description;
    const glm::mat4 at = glm::translate(glm::mat4(1.0F), {7.4F, 0.55F, -11.3F});
    description.objects.push_back(object("room:snes/fx:hero.console", Source::Prefab, at));
    description.objects.push_back(object("room:psx/fx:hero.console", Source::Model, at));       // swapped already
    description.objects.push_back(object("room:snes/fx:hero.console.2", Source::Prefab, at));   // not the part
    description.objects.push_back(object("area:arcade/fx:hero.console", Source::Prefab, at));   // no console room
    description.objects.push_back(object("fx:hero.console", Source::Prefab, at));               // no room
    const auto sites = rv::app::consoleSites(description);
    REQUIRE(sites.size() == 1);
    CHECK(sites.front().room.toString() == "room:snes");
    CHECK(sites.front().objectId == "room:snes/fx:hero.console");
    CHECK(sites.front().transform == at);

    CHECK(rv::app::contentModelPath("snes", "console.glb") == "content:consoles/snes/console.glb");
    CHECK(rv::app::contentModelSystem("content:consoles/snes/console.glb") == std::optional<std::string>("snes"));
    CHECK_FALSE(rv::app::contentModelSystem("models/crt.glb").has_value());
    CHECK_FALSE(rv::app::contentModelSystem("content:consoles//x.glb").has_value());
}

TEST_CASE("app/content_consoles: the model in the console's place, its socket", "[app][content_consoles]") {
    const rv::test::TempDir dir;
    REQUIRE(rv::test::writeFixtureConsole(dir.path() / "console.glb").has_value());
    auto model = rv::assets::loadGltf(dir.path() / "console.glb");
    REQUIRE(model.has_value());
    const rv::assets::ConsoleManifest manifest;
    const rv::app::ConsoleSite site{rv::world::RoomId::parse("room:snes").value(), "room:snes/fx:hero.console",
                                    glm::translate(glm::mat4(1.0F), {1.0F, 0.55F, -4.0F})};

    SECTION("a console within the cabinet's reach stands at the site") {
        const glm::mat4 local = rv::app::consoleModelLocal(*model, manifest, 0.24F);
        const rv::scene::SceneObject placed = rv::app::consoleModelObject(site, "content:consoles/snes/console.glb", local);
        CHECK(placed.id == site.objectId);
        CHECK(placed.source == rv::scene::SceneObject::Source::Model);
        CHECK(placed.modelPath == "content:consoles/snes/console.glb");
        CHECK(placed.collides);
        CHECK(placed.materialsByRole.empty());
        // Its bottom's middle at the site's origin, on the cabinet.
        const glm::vec3 bottom(0.5F * (model->bounds.min.x + model->bounds.max.x), model->bounds.min.y,
                               0.5F * (model->bounds.min.z + model->bounds.max.z));
        const glm::vec3 inScene(placed.transform * glm::vec4(bottom, 1.0F));
        CHECK(inScene.x == Approx(1.0F).margin(1e-5F));
        CHECK(inScene.y == Approx(0.55F).margin(1e-5F));
        CHECK(inScene.z == Approx(-4.0F).margin(1e-5F));
    }
    SECTION("a wider one moves left by what reaches past the cabinet's edge") {
        const glm::mat4 local = rv::app::consoleModelLocal(*model, manifest, 0.40F);
        CHECK(local[3].x - rv::assets::consolePlacement(*model, manifest.front)[3].x ==
              Approx(rv::app::kConsoleRightReach - 0.20F));
    }
    SECTION("the socket from the model's anchor, the CRT's screen kept") {
        const glm::mat4 screen = glm::translate(glm::mat4(1.0F), {0.0F, 1.0F, -4.5F});
        const rv::app::ConsoleSocket socket = rv::app::consoleModelSocket(*model, manifest, site, 0.24F, screen);
        CHECK(socket.position().x == Approx(1.0F).margin(0.01F));
        CHECK(socket.position().y == Approx(0.55F + 0.07F).margin(0.003F));
        CHECK(socket.heroScreen == std::optional<glm::mat4>(screen));
    }
}
