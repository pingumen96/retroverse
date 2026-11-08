// I11 step 5: where a console room's history plaque stands, whether a ray points at it, what it says, and the contract
// of its card's document (app::plaqueSites, plaqueObject, plaqueHit, plaqueText; assets/ui/room_plaque.rml).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/history_plaques.hpp>
#include <rv/app/plaque_card_ui.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

using Catch::Approx;
using rv::app::SignSystem;

namespace {

rv::scene::SceneObject signAt(std::string id, const glm::mat4& transform) {
    rv::scene::SceneObject object;
    object.id = std::move(id);
    object.kind = "poster_frame";
    object.transform = transform;
    return object;
}

std::string readAsset(std::string_view relative) {
    auto text = rv::core::readTextFile(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR) / rv::core::pathFromUtf8(relative));
    REQUIRE(text.has_value());
    return *text;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("app/history_plaques: the sites beside the console rooms' hero corners", "[app][history_plaques]") {
    rv::scene::SceneDescription description;
    // room:snes on the north row (its sign at x 7.025, 2.5 m up, on the wall at z -11.62); room:psx turned round.
    description.objects.push_back(signAt("room:snes/fx:hero.sign", glm::translate(glm::mat4(1.0F), {7.025F, 2.5F, -11.62F})));
    description.objects.push_back(signAt(
        "room:psx/fx:hero.sign",
        glm::rotate(glm::translate(glm::mat4(1.0F), {3.0F, 2.5F, 4.0F}), glm::radians(180.0F), glm::vec3(0, 1, 0))));
    description.objects.push_back(signAt("area:shared:gen4/fx:hero.sign", glm::mat4(1.0F))); // a shared room: none
    description.objects.push_back(signAt("room:snes/fx:hero.sign.2", glm::mat4(1.0F)));        // a pack's piece: none
    description.objects.push_back(signAt("hall:gen4/portal:room:snes/sign", glm::mat4(1.0F)));

    const auto sites = rv::app::plaqueSites(description);
    REQUIRE(sites.size() == 2);
    CHECK(sites[0].room.toString() == "room:snes");
    const glm::vec3 snes(sites[0].transform[3]);
    CHECK(snes.x == Approx(7.98F));
    CHECK(snes.y == Approx(1.35F));
    CHECK(snes.z == Approx(-11.62F));
    // Turned with its room: to the sign's own right.
    const glm::vec3 psx(sites[1].transform[3]);
    CHECK(psx.x == Approx(3.0F - 0.955F));
    CHECK(psx.y == Approx(1.35F));

    const rv::scene::SceneObject plaque = rv::app::plaqueObject(sites[0]);
    CHECK(plaque.id == "plaque:room:snes");
    CHECK(rv::app::plaqueRoom(plaque.id)->toString() == "room:snes");
    CHECK_FALSE(rv::app::plaqueRoom("room:snes/fx:hero.sign").has_value());
    CHECK(plaque.kind == "poster_frame");
    CHECK(plaque.materialsByRole.at("frame") == "MAT_Metal_Brass");
    CHECK(plaque.texturesByRole.at("picture").kind == rv::scene::TextureSource::Kind::Pattern);
    CHECK_FALSE(plaque.collides);
    CHECK_FALSE(plaque.castsShadows);
    CHECK(plaque.params.at("width").get<float>() == Approx(rv::app::kPlaqueWidth));
}

TEST_CASE("app/history_plaques: the pick ray and the plate's box", "[app][history_plaques]") {
    const glm::mat4 at = glm::translate(glm::mat4(1.0F), {0.0F, 1.35F, -5.0F});
    SECTION("from in front, within reach") {
        const auto hit = rv::app::plaqueHit(at, rv::core::Ray{{0.1F, 1.4F, -3.5F}, {0.0F, 0.0F, -1.0F}});
        REQUIRE(hit.has_value());
        CHECK(*hit == Approx(1.5F - rv::app::kPlaqueDepth));
    }
    SECTION("beside it, too far, or behind the eye") {
        CHECK_FALSE(rv::app::plaqueHit(at, rv::core::Ray{{0.3F, 1.4F, -3.5F}, {0.0F, 0.0F, -1.0F}}).has_value());
        CHECK_FALSE(rv::app::plaqueHit(at, rv::core::Ray{{0.0F, 1.35F, -1.0F}, {0.0F, 0.0F, -1.0F}}).has_value());
        CHECK_FALSE(rv::app::plaqueHit(at, rv::core::Ray{{0.0F, 1.35F, -4.0F}, {0.0F, 0.0F, 1.0F}}).has_value());
    }
    const rv::core::Aabb box = rv::app::plaqueBounds(at);
    CHECK(box.min.x == Approx(-0.21F));
    CHECK(box.max.y == Approx(1.5F));
    CHECK(box.max.z == Approx(-5.0F + rv::app::kPlaqueDepth));
}

TEST_CASE("app/history_plaques: what a plaque says, none without a description", "[app][history_plaques]") {
    SignSystem system{"Lumen 16", "Kestrel SNES (Lumen)", "x.svg", std::nullopt, "Fixture Works", "1991", "Console",
                      "Una console inventata."};
    const auto text = rv::app::plaqueText(system);
    REQUIRE(text.has_value());
    CHECK(text->name == "Lumen 16");
    CHECK(text->maker == "Fixture Works");
    CHECK(text->description == "Una console inventata.");
    system.description.clear();
    CHECK_FALSE(rv::app::plaqueText(system).has_value());
}

TEST_CASE("app/plaque_card_document: the ids, the data model and its words", "[app][documents]") {
    namespace rp = rv::app::room_plaque;
    const std::string rml = readAsset(rp::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(rp::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(rp::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    for (const std::string_view id : {rp::kBodyId, rp::kNameId, rp::kDescriptionId, rp::kLogoId, rp::kBodyRegionId}) {
        CAPTURE(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    for (const std::string_view binding : {"{{ name }}", "{{ maker }}", "{{ year }}", "{{ type }}", "{{ description }}"}) {
        CAPTURE(binding);
        CHECK(contains(rml, binding));
    }
    CHECK(contains(rml, "data-attr-src=\"logo\""));
    for (const std::string_view words : {"Produttore", "Anno", "Tipo", "n.d.", "Indietro per chiudere"}) {
        CAPTURE(words);
        CHECK(contains(rml, words));
    }
    const std::string rcss = readAsset(rp::kStyleSheetPath);
    CHECK_FALSE(contains(rcss, "transition"));
}
