// The screens' CRT looks (I5 step 6, ADR-0016 §7): the user's crtEffect over video.json's crtPreset (the screen's, the
// room's, the kind's), over the room's pack's screenPreset, over the kind's default; the screens of a scene and their
// rooms; the materials given their preset's values.

#include <rv/app/crt_looks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using rv::app::CrtChoice;
using rv::app::CrtEffect;
using rv::app::CrtRule;
using rv::app::ScreenKind;
using rv::app::ScreenLooks;
using rv::assets::CrtPreset;

namespace {

/// A prefab object; with `screenPicture` its "screen" role has a scene texture (its screen shows pictures).
rv::scene::SceneObject object(std::string id, std::string kind, bool screenPicture = true) {
    rv::scene::SceneObject result;
    result.id = std::move(id);
    result.kind = std::move(kind);
    result.source = rv::scene::SceneObject::Source::Prefab;
    if (screenPicture) {
        rv::scene::TextureSource source;
        source.kind = rv::scene::TextureSource::Kind::Pattern;
        source.pattern = "standby";
        result.texturesByRole.emplace("screen", source);
    }
    return result;
}

/// A material instance of `key` drawn by object `objectIndex`, with a tube or without.
void addMaterial(rv::app::SceneContentPlan& plan, std::string key, std::size_t objectIndex, bool tube,
                 bool picture = true) {
    rv::app::PlannedMaterial material;
    material.key = std::move(key);
    material.material.shading = rv::assets::ShadingModel::Unlit;
    if (tube) {
        material.material.crt = rv::assets::CrtEffect{};
    }
    if (picture) {
        rv::scene::TextureSource source;
        source.kind = rv::scene::TextureSource::Kind::Pattern;
        source.pattern = "standby";
        material.picture = source;
    }
    plan.materials.push_back(std::move(material));
    rv::app::PlannedItem item;
    item.object = objectIndex;
    item.material = plan.materials.size() - 1;
    plan.items.push_back(item);
}

} // namespace

TEST_CASE("app/crt_looks: the kinds' defaults and vintage's step", "[app][crt_looks]") {
    CHECK(rv::app::defaultCrtPreset(ScreenKind::Hero) == CrtPreset::Consumer);
    CHECK(rv::app::defaultCrtPreset(ScreenKind::Secondary) == CrtPreset::Consumer);
    CHECK(rv::app::defaultCrtPreset(ScreenKind::Cabinet) == CrtPreset::Arcade);
    CHECK(rv::app::strongerCrtPreset(CrtPreset::Neutral) == CrtPreset::Neutral);
    CHECK(rv::app::strongerCrtPreset(CrtPreset::Monitor) == CrtPreset::Consumer);
    CHECK(rv::app::strongerCrtPreset(CrtPreset::Consumer) == CrtPreset::Arcade);
    CHECK(rv::app::strongerCrtPreset(CrtPreset::Arcade) == CrtPreset::Arcade);
    CHECK(rv::app::toString(CrtRule::Kind) == "kind");
    CHECK(rv::app::toString(CrtRule::Pack) == "pack");
    CHECK(rv::app::toString(CrtRule::VideoSettings) == "video.json");
}

TEST_CASE("app/crt_looks: the rules' order as a table", "[app][crt_looks]") {
    struct Row {
        CrtEffect effect;
        std::string_view video;
        std::string_view pack;
        ScreenKind kind;
        CrtPreset preset;
        CrtPreset chosen;
        CrtRule rule;
    };
    using enum CrtPreset;
    const std::vector<Row> rows = {
        // 4. the kind's default.
        {CrtEffect::Light, "", "", ScreenKind::Hero, Consumer, Consumer, CrtRule::Kind},
        {CrtEffect::Light, "", "", ScreenKind::Secondary, Consumer, Consumer, CrtRule::Kind},
        {CrtEffect::Light, "", "", ScreenKind::Cabinet, Arcade, Arcade, CrtRule::Kind},
        // 3. the pack's over the kind's (the lounge's monitor, the arcade's arcade).
        {CrtEffect::Light, "", "monitor", ScreenKind::Hero, Monitor, Monitor, CrtRule::Pack},
        {CrtEffect::Light, "", "monitor", ScreenKind::Secondary, Monitor, Monitor, CrtRule::Pack},
        {CrtEffect::Light, "", "arcade", ScreenKind::Hero, Arcade, Arcade, CrtRule::Pack},
        {CrtEffect::Light, "", "consumer", ScreenKind::Cabinet, Consumer, Consumer, CrtRule::Pack},
        // 2. video.json's over the pack's.
        {CrtEffect::Light, "neutral", "monitor", ScreenKind::Hero, Neutral, Neutral, CrtRule::VideoSettings},
        {CrtEffect::Light, "consumer", "arcade", ScreenKind::Cabinet, Consumer, Consumer, CrtRule::VideoSettings},
        {CrtEffect::Light, "monitor", "", ScreenKind::Secondary, Monitor, Monitor, CrtRule::VideoSettings},
        // A name that is no preset counts as none.
        {CrtEffect::Light, "fancy", "lcd", ScreenKind::Hero, Consumer, Consumer, CrtRule::Kind},
        // 1. off: every screen neutral, whatever chose.
        {CrtEffect::Off, "", "", ScreenKind::Hero, Neutral, Consumer, CrtRule::Kind},
        {CrtEffect::Off, "", "arcade", ScreenKind::Cabinet, Neutral, Arcade, CrtRule::Pack},
        {CrtEffect::Off, "monitor", "arcade", ScreenKind::Hero, Neutral, Monitor, CrtRule::VideoSettings},
        // 1. vintage: one step stronger.
        {CrtEffect::Vintage, "", "", ScreenKind::Hero, Arcade, Consumer, CrtRule::Kind},
        {CrtEffect::Vintage, "", "monitor", ScreenKind::Hero, Consumer, Monitor, CrtRule::Pack},
        {CrtEffect::Vintage, "", "", ScreenKind::Cabinet, Arcade, Arcade, CrtRule::Kind},
        {CrtEffect::Vintage, "neutral", "", ScreenKind::Secondary, Neutral, Neutral, CrtRule::VideoSettings},
    };
    for (const Row& row : rows) {
        INFO(rv::app::toString(row.effect) << " video '" << row.video << "' pack '" << row.pack << "' "
                                           << rv::app::toString(row.kind));
        const CrtChoice choice = rv::app::chooseCrtPreset(row.effect, row.video, row.pack, row.kind);
        CHECK(choice.preset == row.preset);
        CHECK(choice.chosen == row.chosen);
        CHECK(choice.rule == row.rule);
    }
}

TEST_CASE("app/crt_looks: video.json's keys by screen, room and kind, the scene's packs", "[app][crt_looks]") {
    rv::app::VideoSettings video;
    video.secondary.crtPreset = "monitor";
    rv::app::RoomVideoOverrides lounge;
    lounge.hero = rv::app::ScreenOverrides{.crtPreset = std::string("arcade")};
    lounge.screens.emplace("room:gen6/fx:side.secondary_tv", rv::app::ScreenOverrides{.crtPreset = std::string("neutral")});
    video.rooms.emplace("room:gen6", lounge);
    rv::app::RoomVideoOverrides snes;
    snes.hero = rv::app::ScreenOverrides{.crtPreset = std::string()}; // "" in a room: none, its kind's key
    video.rooms.emplace("room:snes", snes);

    ScreenLooks looks;
    looks.setVideo(video);
    looks.setScene({{"room:gen6", "monitor"}, {"area:arcade", "arcade"}});

    // The room's kind key, over the pack's.
    CHECK(looks.choose(ScreenKind::Hero, "room:gen6", "room:gen6/fx:hero.crt") ==
          CrtChoice{CrtPreset::Arcade, CrtPreset::Arcade, CrtRule::VideoSettings});
    // The screen's key, over its room's kind's.
    CHECK(looks.choose(ScreenKind::Secondary, "room:gen6", "room:gen6/fx:side.secondary_tv") ==
          CrtChoice{CrtPreset::Neutral, CrtPreset::Neutral, CrtRule::VideoSettings});
    // The kind's key (the user's for every secondary), over the pack's.
    CHECK(looks.choose(ScreenKind::Secondary, "room:gen6", "room:gen6/fx:other.secondary_tv") ==
          CrtChoice{CrtPreset::Monitor, CrtPreset::Monitor, CrtRule::VideoSettings});
    // The pack's where video.json says nothing.
    CHECK(looks.choose(ScreenKind::Hero, "area:arcade", "area:arcade/fx:hero.crt") ==
          CrtChoice{CrtPreset::Arcade, CrtPreset::Arcade, CrtRule::Pack});
    CHECK(looks.choose(ScreenKind::Hero, "room:snes", "room:snes/fx:hero.crt") ==
          CrtChoice{CrtPreset::Consumer, CrtPreset::Consumer, CrtRule::Kind});
    // A pack given explicitly (a collection's: its scene data's), "" for none.
    CHECK(looks.choose(ScreenKind::Cabinet, "area:arcade", {}, std::string_view("consumer")).preset ==
          CrtPreset::Consumer);
    CHECK(looks.choose(ScreenKind::Cabinet, "area:arcade", {}, std::string_view()) ==
          CrtChoice{CrtPreset::Arcade, CrtPreset::Arcade, CrtRule::Kind});
    // The user's crtEffect over all of it.
    video.crtEffect = CrtEffect::Off;
    looks.setVideo(video);
    CHECK(looks.choose(ScreenKind::Hero, "room:gen6", "room:gen6/fx:hero.crt").preset == CrtPreset::Neutral);
    CHECK(looks.choose(ScreenKind::Hero, "area:arcade", "area:arcade/fx:hero.crt").preset == CrtPreset::Neutral);
    video.crtEffect = CrtEffect::Vintage;
    looks.setVideo(video);
    CHECK(looks.choose(ScreenKind::Hero, "room:snes", "room:snes/fx:hero.crt").preset == CrtPreset::Arcade);
    CHECK(looks.choose(ScreenKind::Secondary, "room:snes", "room:snes/fx:a.secondary_tv").preset ==
          CrtPreset::Consumer);
    // Each choice's values are its preset's.
    const CrtChoice arcade{CrtPreset::Arcade, CrtPreset::Consumer, CrtRule::Kind};
    CHECK(looks.effect(arcade) == rv::assets::builtInCrtPresets()[CrtPreset::Arcade]);
}

TEST_CASE("app/crt_looks: a room's or a screen's empty crtPreset leaves the kind's", "[app][crt_looks]") {
    // ADR-0016 §7: "" is none. The user's kind key (arcade for every hero) stays under a room's and a screen's "".
    rv::app::VideoSettings video;
    video.hero.crtPreset = "arcade";
    rv::app::RoomVideoOverrides snes;
    snes.hero = rv::app::ScreenOverrides{.crtPreset = std::string()};
    snes.screens.emplace("room:snes/fx:hero.crt", rv::app::ScreenOverrides{.crtPreset = std::string()});
    video.rooms.emplace("room:snes", snes);
    rv::app::RoomVideoOverrides lounge;
    lounge.screens.emplace("room:gen6/fx:hero.crt", rv::app::ScreenOverrides{.crtPreset = std::string()});
    video.rooms.emplace("room:gen6", lounge);

    CHECK(video.screen(ScreenKind::Hero, "room:snes").crtPreset == "arcade");
    CHECK(video.screen(ScreenKind::Hero, "room:snes", "room:snes/fx:hero.crt").crtPreset == "arcade");
    CHECK(video.screen(ScreenKind::Hero, "room:gen6", "room:gen6/fx:hero.crt").crtPreset == "arcade");

    ScreenLooks looks;
    looks.setVideo(video);
    looks.setScene({{"room:snes", "monitor"}, {"room:gen6", "monitor"}});
    // The kind's key (arcade) over the pack's, whatever the room's and the screen's "".
    CHECK(looks.choose(ScreenKind::Hero, "room:snes", "room:snes/fx:hero.crt") ==
          CrtChoice{CrtPreset::Arcade, CrtPreset::Arcade, CrtRule::VideoSettings});
    CHECK(looks.choose(ScreenKind::Hero, "room:gen6", "room:gen6/fx:hero.crt") ==
          CrtChoice{CrtPreset::Arcade, CrtPreset::Arcade, CrtRule::VideoSettings});
    // Without the kind's key, the room's "" leaves the pack's.
    video.hero.crtPreset.clear();
    looks.setVideo(video);
    CHECK(looks.choose(ScreenKind::Hero, "room:snes", "room:snes/fx:hero.crt") ==
          CrtChoice{CrtPreset::Monitor, CrtPreset::Monitor, CrtRule::Pack});
}

TEST_CASE("app/crt_looks: a scene's screens and their rooms", "[app][crt_looks]") {
    CHECK(rv::app::screenRoomOf("room:snes/fx:hero.crt", "gen4") == "room:snes");
    CHECK(rv::app::screenRoomOf("area:arcade/fx:hero.crt", "ground") == "area:arcade");
    CHECK(rv::app::screenRoomOf("fx:hero.crt", "room:snes") == "room:snes");
    CHECK(rv::app::screenRoomOf("crt_a", "screens_four") == "screens_four");

    CHECK(rv::app::holdsHeroRole(object("room:snes/fx:hero.crt", "crt")));
    CHECK(rv::app::holdsHeroRole(object("area:arcade/fx:hero.crt", "arcade_cabinet")));
    CHECK_FALSE(rv::app::holdsHeroRole(object("crt_a", "crt")));
    CHECK_FALSE(rv::app::holdsHeroRole(object("room:snes/fx:hero.crt", "desk")));
    CHECK(rv::app::screenShowsPictures(object("crt_a", "crt")));
    CHECK_FALSE(rv::app::screenShowsPictures(object("crt_a", "crt", false)));
    rv::scene::SceneObject primitive = object("crt_a", "crt");
    primitive.source = rv::scene::SceneObject::Source::Primitive;
    CHECK_FALSE(rv::app::screenShowsPictures(primitive));

    CHECK(rv::app::screenKindOf(object("room:snes/fx:hero.crt", "crt"), true) == ScreenKind::Hero);
    CHECK(rv::app::screenKindOf(object("area:arcade/fx:hero.crt", "arcade_cabinet"), true) == ScreenKind::Hero);
    CHECK(rv::app::screenKindOf(object("crt_a", "crt"), true) == ScreenKind::Hero);
    CHECK(rv::app::screenKindOf(object("crt_b", "crt"), false) == ScreenKind::Secondary);
    CHECK(rv::app::screenKindOf(object("room:snes/fx:hero.crt", "crt"), false) == ScreenKind::Secondary);
    CHECK(rv::app::screenKindOf(object("room:snes/fx:side.0", "secondary_tv"), false) == ScreenKind::Secondary);
    CHECK_FALSE(rv::app::screenKindOf(object("area:arcade/fx:hero.crt", "arcade_cabinet"), false).has_value());
    CHECK_FALSE(rv::app::screenKindOf(object("area:arcade/fx:row.0", "arcade_cabinet"), false).has_value());
    CHECK_FALSE(rv::app::screenKindOf(object("room:snes/fx:desk", "desk"), false).has_value());
}

TEST_CASE("app/crt_looks: the hero is the first screen that binds, by its role, else the first crt",
          "[app][crt_looks]") {
    const std::vector<rv::scene::SceneObject> objects = {
        object("crt_a", "crt"),                           // 0: a crt before the role's holder
        object("area:arcade/fx:row.0", "arcade_cabinet"), // 1: never asked
        object("fx:hero.crt", "crt"),                     // 2: the role's holder
        object("crt_b", "crt"),                           // 3
    };
    std::vector<std::size_t> asked;
    const auto binds = [&asked](std::vector<std::size_t> bound) {
        return [&asked, bound](std::size_t index) {
            asked.push_back(index);
            return std::find(bound.begin(), bound.end(), index) != bound.end();
        };
    };
    // The role's holder that binds, over a crt before it.
    CHECK(rv::app::heroScreenIndex(objects, binds({0, 2, 3})) == std::optional<std::size_t>(2));
    CHECK(asked == std::vector<std::size_t>{0, 2});
    // The role's holder that cannot bind: the first crt that binds.
    asked.clear();
    CHECK(rv::app::heroScreenIndex(objects, binds({3})) == std::optional<std::size_t>(3));
    CHECK(asked == std::vector<std::size_t>{0, 2, 3});
    // None binds: none.
    asked.clear();
    CHECK_FALSE(rv::app::heroScreenIndex(objects, binds({})).has_value());
    CHECK(asked == std::vector<std::size_t>{0, 2, 3});
}

TEST_CASE("app/crt_looks: the screens' material instances take their looks", "[app][crt_looks]") {
    // A floor: the lounge's hero and secondary (its pack's monitor), the SNES room's hero (consumer), the arcade's
    // hero cabinet (arcade), an arcade row cabinet's screen back (no tube), a desk's monitor (no tube), a poster.
    const std::vector<rv::scene::SceneObject> objects = {
        object("room:gen6/fx:hero.crt", "crt"),
        object("room:gen6/fx:side.secondary_tv", "secondary_tv"),
        object("room:snes/fx:hero.crt", "crt"),
        object("area:arcade/fx:hero.crt", "arcade_cabinet"),
        object("area:arcade/fx:row.0", "arcade_cabinet"),
        object("room:gen6/fx:desk", "desk"),
        object("room:snes/fx:poster", "poster_frame"),
    };
    rv::app::SceneContentPlan plan;
    addMaterial(plan, "room:gen6/fx:hero.crt/screen@640x480", 0, true);
    addMaterial(plan, "room:gen6/fx:hero.crt/plastic_body", 0, false, false);
    addMaterial(plan, "room:gen6/fx:side.secondary_tv/screen@320x240", 1, true);
    addMaterial(plan, "room:snes/fx:hero.crt/screen@640x480", 2, true);
    addMaterial(plan, "area:arcade/fx:hero.crt/screen@640x480", 3, true);
    addMaterial(plan, "area:arcade/fx:row.0/screen_back", 4, false, false);
    addMaterial(plan, "room:gen6/fx:desk/screen@320x200", 5, false);
    addMaterial(plan, "room:snes/fx:poster/picture@300x400", 6, false);

    rv::app::VideoSettings video;
    ScreenLooks looks;
    looks.setVideo(video);
    looks.setScene({{"room:gen6", "monitor"}, {"area:arcade", "arcade"}});
    CHECK(rv::app::applyScreenLooks(plan, objects, "gen6", looks) == 4);

    const rv::assets::CrtPresets& presets = rv::assets::builtInCrtPresets();
    CHECK(plan.materials[0].material.crt == presets[CrtPreset::Monitor]);
    CHECK_FALSE(plan.materials[1].material.crt.has_value());
    CHECK(plan.materials[2].material.crt == presets[CrtPreset::Monitor]);
    CHECK(plan.materials[3].material.crt == presets[CrtPreset::Consumer]);
    CHECK(plan.materials[4].material.crt == presets[CrtPreset::Arcade]);
    CHECK_FALSE(plan.materials[5].material.crt.has_value());
    CHECK_FALSE(plan.materials[6].material.crt.has_value());
    CHECK_FALSE(plan.materials[7].material.crt.has_value());

    const std::vector<rv::app::ScreenLookApplied> applied = looks.applied();
    REQUIRE(applied.size() == 4);
    CHECK(applied[0] == rv::app::ScreenLookApplied{"area:arcade/fx:hero.crt", "area:arcade", ScreenKind::Hero,
                                                   {CrtPreset::Arcade, CrtPreset::Arcade, CrtRule::Pack}});
    CHECK(applied[1] == rv::app::ScreenLookApplied{"room:gen6/fx:hero.crt", "room:gen6", ScreenKind::Hero,
                                                   {CrtPreset::Monitor, CrtPreset::Monitor, CrtRule::Pack}});
    CHECK(applied[2] == rv::app::ScreenLookApplied{"room:gen6/fx:side.secondary_tv", "room:gen6",
                                                   ScreenKind::Secondary,
                                                   {CrtPreset::Monitor, CrtPreset::Monitor, CrtRule::Pack}});
    CHECK(applied[3] == rv::app::ScreenLookApplied{"room:snes/fx:hero.crt", "room:snes", ScreenKind::Hero,
                                                   {CrtPreset::Consumer, CrtPreset::Consumer, CrtRule::Kind}});

    // A new scene forgets the looks recorded.
    looks.setScene({});
    CHECK(looks.applied().empty());
}

TEST_CASE("app/crt_looks: a hand-made scene's first crt is its hero", "[app][crt_looks]") {
    const std::vector<rv::scene::SceneObject> objects = {
        object("room", "room_shell"), object("crt_a", "crt"), object("crt_b", "crt")};
    rv::app::SceneContentPlan plan;
    addMaterial(plan, "crt_a/screen@640x480", 1, true);
    addMaterial(plan, "crt_b/screen@640x480", 2, true);
    rv::app::VideoSettings video;
    video.hero.crtPreset = "neutral";
    video.secondary.crtPreset = "arcade";
    ScreenLooks looks;
    looks.setVideo(video);
    CHECK(rv::app::applyScreenLooks(plan, objects, "screens_four", looks) == 2);
    const rv::assets::CrtPresets& presets = rv::assets::builtInCrtPresets();
    CHECK(plan.materials[0].material.crt == presets[CrtPreset::Neutral]);
    CHECK(plan.materials[1].material.crt == presets[CrtPreset::Arcade]);
    const std::vector<rv::app::ScreenLookApplied> applied = looks.applied();
    REQUIRE(applied.size() == 2);
    CHECK(applied[0].room == "screens_four");
    CHECK(applied[0].kind == ScreenKind::Hero);
    CHECK(applied[1].kind == ScreenKind::Secondary);
}

TEST_CASE("app/crt_looks: a first crt that cannot bind is no hero", "[app][crt_looks]") {
    // A hand-made scene whose first crt's screen shows no picture (no "screen" texture: the screens' service never
    // binds it), then two crts that bind: the hero is the second object, as findScreens() binds it. On a floor, a room
    // whose role's holder cannot bind takes its first crt that binds; another room keeps its role's holder.
    const std::vector<rv::scene::SceneObject> objects = {
        object("room", "room_shell"),
        object("crt_a", "crt", false),
        object("crt_b", "crt"),
        object("crt_c", "crt"),
    };
    rv::app::SceneContentPlan plan;
    addMaterial(plan, "crt_a/screen@640x480", 1, true);
    addMaterial(plan, "crt_b/screen@640x480", 2, true);
    addMaterial(plan, "crt_c/screen@640x480", 3, true);
    rv::app::VideoSettings video;
    video.hero.crtPreset = "neutral";
    video.secondary.crtPreset = "arcade";
    ScreenLooks looks;
    looks.setVideo(video);
    CHECK(rv::app::applyScreenLooks(plan, objects, "screens_four", looks) == 3);
    const rv::assets::CrtPresets& presets = rv::assets::builtInCrtPresets();
    CHECK(plan.materials[0].material.crt == presets[CrtPreset::Arcade]);
    CHECK(plan.materials[1].material.crt == presets[CrtPreset::Neutral]);
    CHECK(plan.materials[2].material.crt == presets[CrtPreset::Arcade]);
    const std::vector<rv::app::ScreenLookApplied> applied = looks.applied();
    REQUIRE(applied.size() == 3);
    CHECK(applied[0].screen == "crt_a");
    CHECK(applied[0].kind == ScreenKind::Secondary);
    CHECK(applied[1].screen == "crt_b");
    CHECK(applied[1].kind == ScreenKind::Hero);
    CHECK(applied[2].kind == ScreenKind::Secondary);

    const std::vector<rv::scene::SceneObject> floor = {
        object("room:gen6/fx:hero.crt", "crt", false),
        object("room:gen6/fx:side.crt_extra", "crt"),
        object("room:snes/fx:side.0", "crt"),
        object("room:snes/fx:hero.crt", "crt"),
    };
    rv::app::SceneContentPlan floorPlan;
    addMaterial(floorPlan, "room:gen6/fx:hero.crt/screen@640x480", 0, true);
    addMaterial(floorPlan, "room:gen6/fx:side.crt_extra/screen@640x480", 1, true);
    addMaterial(floorPlan, "room:snes/fx:side.0/screen@640x480", 2, true);
    addMaterial(floorPlan, "room:snes/fx:hero.crt/screen@640x480", 3, true);
    looks.setScene({});
    CHECK(rv::app::applyScreenLooks(floorPlan, floor, "gen6", looks) == 4);
    CHECK(floorPlan.materials[0].material.crt == presets[CrtPreset::Arcade]);  // its role's, unbound: a secondary
    CHECK(floorPlan.materials[1].material.crt == presets[CrtPreset::Neutral]); // its room's first crt that binds
    CHECK(floorPlan.materials[2].material.crt == presets[CrtPreset::Arcade]);  // a crt before its room's hero
    CHECK(floorPlan.materials[3].material.crt == presets[CrtPreset::Neutral]); // its room's role's holder
}
