// I11 step 4: which objects of a scene are the rooms' signs, and what each shows (app::identitySignTargets,
// identitySignContent).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/identity_signs.hpp>

using rv::app::IdentitySignContent;
using rv::app::IdentitySignPlace;
using rv::app::SignSystem;
using rv::assets::IdentitySignStyle;
using rv::catalog::SystemName;
using rv::world::RoomId;

namespace {

RoomId room(std::string_view text) {
    auto parsed = RoomId::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

SystemName systemNamed(std::string_view name) {
    auto parsed = SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

rv::scene::SceneObject signObject(std::string id, std::string pattern, std::string role = "picture") {
    rv::scene::SceneObject object;
    object.id = std::move(id);
    rv::scene::TextureSource source;
    source.kind = rv::scene::TextureSource::Kind::Pattern;
    source.pattern = std::move(pattern);
    source.title = "16-BIT CORNER";
    object.texturesByRole.emplace(std::move(role), source);
    return object;
}

/// The invented systems of the tests: snes with a theme's name and logo, megadrive with a logo and ES-DE's name,
/// n64 with a name of its identity and no logo, psx with nothing of its own.
std::optional<SignSystem> lookup(const SystemName& name) {
    static const std::map<std::string, SignSystem> systems{
        {"snes", SignSystem{"Lumen 16", "Kestrel SNES (Lumen)", "C:/themes/snes.svg"}},
        {"megadrive", SignSystem{"Orbit Drive (Mega)", "Orbit Drive (Mega)", "C:/themes/megadrive.png"}},
        {"n64", SignSystem{"Polygon 64", "Kestrel 64", ""}},
        {"psx", SignSystem{"Sony PlayStation", "Sony PlayStation", ""}},
    };
    const auto found = systems.find(name.str());
    if (found == systems.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace

TEST_CASE("app/identity_signs: the signs' objects and the rooms they name", "[app][identity_signs]") {
    SECTION("the room's neon, its pack's further pieces") {
        const auto hero = rv::app::identitySignRoom("room:snes/fx:hero.sign");
        REQUIRE(hero.has_value());
        CHECK(hero->first == room("room:snes"));
        CHECK(hero->second == IdentitySignPlace::Hero);
        CHECK(rv::app::identitySignRoom("room:snes:2/fx:hero.sign.2")->first == room("room:snes:2"));
        CHECK_FALSE(rv::app::identitySignRoom("room:snes/fx:hero.signpost").has_value());
        CHECK_FALSE(rv::app::identitySignRoom("room:snes/fx:hero.crt").has_value());
        CHECK_FALSE(rv::app::identitySignRoom("fx:hero.sign").has_value()); // a room's own scene: no room named
    }
    SECTION("the hall's portals, their blades, the hub's portals and fascia") {
        for (const std::string_view id :
             {"hall:gen4/portal:room:snes/sign", "hall:gen4/portal:room:snes/blade-east",
              "hall:gen4/portal:room:snes/blade-west", "hall:gen4/hub/portal:room:snes/sign",
              "hall:gen4/hub/fascia-sign:room:snes"}) {
            const auto hall = rv::app::identitySignRoom(id);
            REQUIRE(hall.has_value());
            CHECK(hall->first == room("room:snes"));
            CHECK(hall->second == IdentitySignPlace::Hall);
        }
        CHECK(rv::app::identitySignRoom("hall:gen4/portal:area:shared:gen4:2/sign")->first ==
              room("area:shared:gen4:2"));
        CHECK(rv::app::identitySignRoom("hall:ground/hub/fascia-sign:area:arcade")->first == room("area:arcade"));
    }
    SECTION("what is not a room's sign") {
        for (const std::string_view id :
             {"hall:gen4/sign", "hall:gen4/hub/portal:entrance/sign", "hall:gen4/hub/fascia-sign:entrance",
              "hall:gen4/portal:room:snes/frame", "hall:gen4/hub/totem", "hall:gen4/portal:room:snes/sign/x",
              "room:snes/dressing:sign-icon", "hall:gen4/portal:not a room/sign"}) {
            CAPTURE(id);
            CHECK_FALSE(rv::app::identitySignRoom(id).has_value());
        }
    }
    SECTION("a scene's signs: a neon or a sign pattern on the picture, in the objects' order") {
        rv::scene::SceneDescription description;
        description.objects.push_back(signObject("room:snes/fx:hero.sign", "neon"));
        description.objects.push_back(signObject("hall:gen4/portal:room:snes/sign", "sign"));
        description.objects.push_back(signObject("hall:gen4/hub/fascia-sign:room:psx", "neon"));
        description.objects.push_back(signObject("hall:gen4/sign", "sign"));                     // the floor's
        description.objects.push_back(signObject("room:psx/fx:hero.sign", "cover"));             // another pattern
        description.objects.push_back(signObject("room:n64/fx:hero.sign", "neon", "frame"));     // another role
        rv::scene::SceneObject file = signObject("room:gb/fx:hero.sign", "neon");
        file.texturesByRole.at("picture").kind = rv::scene::TextureSource::Kind::File;
        description.objects.push_back(file);
        const auto targets = rv::app::identitySignTargets(description);
        REQUIRE(targets.size() == 3);
        CHECK(targets[0].objectId == "room:snes/fx:hero.sign");
        CHECK(targets[0].style == IdentitySignStyle::Neon);
        CHECK(targets[0].place == IdentitySignPlace::Hero);
        CHECK(targets[1].style == IdentitySignStyle::Lit);
        CHECK(targets[1].place == IdentitySignPlace::Hall);
        CHECK(targets[2].room == room("room:psx"));
        CHECK(targets[2].style == IdentitySignStyle::Neon);
    }
}

TEST_CASE("app/identity_signs: what a room's sign shows", "[app][identity_signs]") {
    SECTION("a system with an identity: its logo alone, else its name; an expansion's number after either") {
        const auto hero = rv::app::identitySignContent(room("room:snes"), IdentitySignPlace::Hero, {}, lookup);
        REQUIRE(hero.has_value());
        CHECK(*hero == IdentitySignContent{{"C:/themes/snes.svg"}, ""});
        const auto second = rv::app::identitySignContent(room("room:snes:2"), IdentitySignPlace::Hall, {}, lookup);
        REQUIRE(second.has_value());
        CHECK(*second == IdentitySignContent{{"C:/themes/snes.svg"}, "2"});
        // A logo with ES-DE's own name: the logo.
        CHECK(*rv::app::identitySignContent(room("room:megadrive"), IdentitySignPlace::Hero, {}, lookup) ==
              IdentitySignContent{{"C:/themes/megadrive.png"}, ""});
        // A name of its identity without a logo: the name.
        const auto named = rv::app::identitySignContent(room("room:n64"), IdentitySignPlace::Hero, {}, lookup);
        REQUIRE(named.has_value());
        CHECK(named->logos.empty());
        CHECK(named->name == "Polygon 64");
        CHECK(rv::app::identitySignContent(room("room:n64:3"), IdentitySignPlace::Hall, {}, lookup)->name ==
              "Polygon 64 3");
    }
    SECTION("a system without an identity, or unknown: the generic sign") {
        CHECK_FALSE(rv::app::identitySignContent(room("room:psx"), IdentitySignPlace::Hero, {}, lookup).has_value());
        CHECK_FALSE(rv::app::identitySignContent(room("room:gb"), IdentitySignPlace::Hall, {}, lookup).has_value());
        CHECK_FALSE(rv::app::identitySignContent(room("room:snes"), IdentitySignPlace::Hall, {}, {}).has_value());
        CHECK_FALSE(rv::app::hasIdentity(SignSystem{"snes", "", ""}));
        CHECK(rv::app::hasIdentity(SignSystem{"snes", "", "x.svg"}));
    }
    SECTION("a shared room: its sections' logos on the hall's signs, its own neon generic") {
        const std::vector<SystemName> sections{systemNamed("psx"), systemNamed("snes"), systemNamed("n64"), systemNamed("megadrive")};
        const auto hall =
            rv::app::identitySignContent(room("area:shared:gen4"), IdentitySignPlace::Hall, sections, lookup);
        REQUIRE(hall.has_value());
        CHECK(*hall == IdentitySignContent{{"C:/themes/snes.svg", "C:/themes/megadrive.png"}, ""});
        CHECK(rv::app::identitySignContent(room("area:shared:gen4:3"), IdentitySignPlace::Hall, sections, lookup)
                  ->name == "3");
        CHECK_FALSE(rv::app::identitySignContent(room("area:shared:gen4"), IdentitySignPlace::Hero, sections, lookup)
                        .has_value());
        const std::vector<SystemName> plain{systemNamed("psx"), systemNamed("n64")};
        CHECK_FALSE(
            rv::app::identitySignContent(room("area:arcade"), IdentitySignPlace::Hall, plain, lookup).has_value());
    }
}
