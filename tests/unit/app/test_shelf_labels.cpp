// I12 step 5 (ADR-0021 §1): the shelves' detail, GPU-free (shelf_labels.hpp): the letter tabs' runs, the caption
// cards, the section signs, where each stands, and the scene objects they are.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/shelf_labels.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>

#include "../world/plan_scene_support.hpp"

namespace {

using rv::app::RoomShelfTexts;
using rv::app::ShelfLabels;
using rv::app::ShelfText;
using rv::test::world::planCase;

/// The texts of `layout`'s games: "Title <n>" filed under the letters in turn (every fifth under a digit), "<year> ·
/// Studio <n>", the section by the game's half of the layout.
RoomShelfTexts textsOf(const rv::world::CollectionLayout& layout, std::size_t sections = 1) {
    RoomShelfTexts texts;
    std::vector<rv::catalog::GameId> games;
    for (const rv::world::CollectionItem& item : layout.items) {
        games.push_back(item.game);
    }
    std::sort(games.begin(), games.end());
    games.erase(std::unique(games.begin(), games.end()), games.end());
    for (std::size_t i = 0; i < games.size(); ++i) {
        ShelfText text;
        text.game = games[i];
        text.title = "Title " + std::to_string(i);
        text.detail = rv::app::captionDetail("1993", "Studio " + std::to_string(i), "");
        text.sortKey = i % 5 == 4 ? std::to_string(i) : std::string(1, static_cast<char>('a' + i % 26)) + "title";
        text.initial = rv::app::shelfInitial(text.sortKey);
        text.section = i * sections / games.size();
        texts.games.push_back(std::move(text));
    }
    for (std::size_t s = 0; s < sections; ++s) {
        texts.sectionNames.push_back("Fixture System " + std::to_string(s));
    }
    return texts;
}

rv::world::CollectionLayout layoutOf(const rv::test::world::PlanCase& entry, const rv::world::WorldPlan& plan,
                                     const rv::world::RoomPlacement& placement = {}) {
    auto layout = rv::world::collectionLayout(plan, entry.room, entry.data, entry.titles);
    REQUIRE(layout.has_value());
    return rv::world::floorCollectionLayout(std::move(*layout), placement);
}

const rv::world::ScenePrefab& prefabOf(const rv::test::world::PlanCase& entry, const rv::world::FixtureId& id) {
    const auto fixture = std::find_if(entry.plan.fixtures.begin(), entry.plan.fixtures.end(),
                                      [&](const rv::world::Fixture& candidate) { return candidate.id == id; });
    REQUIRE(fixture != entry.plan.fixtures.end());
    const rv::world::ScenePrefab* prefab = entry.data.prefabs.find(fixture->prefab);
    REQUIRE(prefab != nullptr);
    return *prefab;
}

glm::vec3 origin(const glm::mat4& frame) {
    return glm::vec3(frame[3]);
}

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return glm::length(a - b) <= tolerance;
}

} // namespace

TEST_CASE("app/shelf_labels: initials, runs and captions' lines", "[app][shelf_labels]") {
    CHECK(rv::app::shelfInitial("lumen quest") == "L");
    CHECK(rv::app::shelfInitial("Quest") == "Q");
    CHECK(rv::app::shelfInitial("1942") == "0-9");
    CHECK(rv::app::shelfInitial("[bios]") == "#");
    CHECK(rv::app::shelfInitial("\xc3\xa9t\xc3\xa9") == "#"); // é
    CHECK(rv::app::shelfInitial("") == "#");
    CHECK(rv::app::initialRank("#") < rv::app::initialRank("0-9"));
    CHECK(rv::app::initialRank("0-9") < rv::app::initialRank("A"));
    CHECK(rv::app::initialRank("A") < rv::app::initialRank("Z"));
    CHECK(rv::app::letterRun("D", "D") == "D");
    CHECK(rv::app::letterRun("A", "C") == "A–C");
    CHECK(rv::app::letterRun("0-9", "0-9") == "0-9");
    CHECK(rv::app::letterRun("0-9", "C") == "0–C");
    CHECK(rv::app::letterRun("#", "0-9") == "#–9");
    CHECK(rv::app::captionDetail("1993", "Fixture Works", "Fixture Press") == "1993 · Fixture Works");
    CHECK(rv::app::captionDetail("1993", "", "Fixture Press") == "1993 · Fixture Press");
    CHECK(rv::app::captionDetail("", "Fixture Works", "") == "Fixture Works");
    CHECK(rv::app::captionDetail("1993", "", "") == "1993");
    CHECK(rv::app::captionDetail("", "", "").empty());

    // A game's text: its title (else its file name's), its screen's sort key (else the title's folded).
    rv::app::RoomItemText item;
    item.game = rv::catalog::GameId::fromValue(7).value();
    item.fallbackTitle = "Lumen Quest (USA)";
    item.year = "1993";
    item.publisher = "Fixture Press";
    ShelfText text = rv::app::shelfTextOf(item, 2);
    CHECK(text.title == "Lumen Quest (USA)");
    CHECK(text.detail == "1993 · Fixture Press");
    CHECK(text.initial == "L");
    CHECK(text.section == 2u);
    item.title = "The Quest";
    rv::app::RoomItemScreen screen;
    screen.sortKey = "quest, the";
    item.screen = screen;
    text = rv::app::shelfTextOf(item, std::nullopt);
    CHECK(text.title == "The Quest");
    CHECK(text.sortKey == "quest, the");
    CHECK(text.initial == "Q");
    CHECK_FALSE(text.section.has_value());
}

TEST_CASE("app/shelf_labels: a console room's tabs and cards", "[app][shelf_labels]") {
    const auto& entry = planCase("plan-2000-seed-102938");
    const rv::world::CollectionLayout layout = layoutOf(entry, entry.plan);
    const RoomShelfTexts texts = textsOf(layout);
    const ShelfLabels labels = rv::app::roomShelfLabels(entry.plan, entry.room, entry.data, layout, {}, texts);
    CHECK(labels.room == entry.room);
    CHECK(labels.sections.empty()); // a console room's
    CHECK(labels.tabMaterials == entry.data.theme.materials("label_plates"));
    CHECK(labels.cardMaterial == "MAT_Paper_Print");

    // A tab per spine-out level of a module with an item, its run from the level's least and greatest initials.
    std::map<std::pair<rv::world::FixtureId, std::uint32_t>, std::pair<std::string, std::string>> runs;
    std::size_t captions = 0;
    for (const rv::world::CollectionItem& item : layout.items) {
        const ShelfText* text = texts.find(item.game);
        REQUIRE(text != nullptr);
        const std::string& kind = prefabOf(entry, item.slot.fixture()).generator;
        if (kind == "shelf" && item.stance == rv::world::ItemStance::SpineOut) {
            auto [run, added] =
                runs.try_emplace({item.slot.fixture(), item.slot.level()}, text->initial, text->initial);
            if (rv::app::initialRank(text->initial) < rv::app::initialRank(run->second.first)) {
                run->second.first = text->initial;
            }
            if (rv::app::initialRank(text->initial) > rv::app::initialRank(run->second.second)) {
                run->second.second = text->initial;
            }
        }
        captions += kind == "display_case" && item.stance == rv::world::ItemStance::FaceOut ? 1 : 0;
    }
    REQUIRE(runs.size() > 10);
    REQUIRE(labels.tabs.size() == runs.size());
    REQUIRE(captions > 0);
    REQUIRE(labels.captions.size() == captions);
    for (const rv::app::ShelfTab& tab : labels.tabs) {
        INFO(tab.fixture.toString() << " level " << tab.level);
        const auto& run = runs.at({tab.fixture, tab.level});
        CHECK(tab.text == rv::app::letterRun(run.first, run.second));
        // At the level's board: its top 3 mm over the slots' bottom, in front of the level's items, near its left end.
        const glm::vec3 at = origin(tab.frame);
        const rv::world::CollectionItem* first = nullptr;
        for (const rv::world::CollectionItem& item : layout.items) {
            if (item.slot.fixture() != tab.fixture || item.slot.level() != tab.level) {
                continue;
            }
            CHECK_FALSE(item.worldBounds().contains(at));
            if (first == nullptr || item.slot.index() < first->slot.index()) {
                first = &item;
            }
        }
        REQUIRE(first != nullptr);
        CHECK(std::abs(at.y + 0.5f * rv::app::kTabSize.y - rv::app::kTabRise - first->slotFrame[3].y) < 1.0e-4f);
        // Facing as the module does: the tab's +Z its items' +Z, and it stands in front of them.
        const glm::vec3 out(first->slotFrame[2]);
        CHECK(glm::dot(glm::vec3(tab.frame[2]), out) > 0.999f);
        CHECK(glm::dot(at - origin(first->slotFrame), out) > 0.0f);
    }
    for (const rv::app::ShelfCaption& caption : labels.captions) {
        INFO(caption.game.toString());
        const rv::world::CollectionItem* item = layout.findGame(caption.game);
        REQUIRE(item != nullptr);
        const ShelfText* text = texts.find(caption.game);
        CHECK(caption.text.title == text->title);
        CHECK(caption.text.detail == text->detail);
        CHECK(caption.size.x <= rv::app::kCaptionSize.x);
        CHECK(caption.size.x / caption.size.y == Catch::Approx(rv::app::kCaptionSize.x / rv::app::kCaptionSize.y));
        // Under the item, leaning with it (the display case's tilt), in front of its face.
        const glm::vec3 out(item->slotFrame[2]);
        CHECK(glm::dot(glm::vec3(caption.frame[2]), out) > 0.999f);
        CHECK(near(origin(caption.frame), origin(item->slotFrame), 0.05f));
        CHECK(glm::dot(origin(caption.frame) - origin(item->slotFrame), out) > 0.0f);
    }
    // The same inputs, the same labels.
    CHECK(rv::app::roomShelfLabels(entry.plan, entry.room, entry.data, layout, {}, texts) == labels);
    // A game whose text is not known: no card, and its level's run without it.
    RoomShelfTexts fewer = texts;
    fewer.games.clear();
    CHECK(rv::app::roomShelfLabels(entry.plan, entry.room, entry.data, layout, {}, fewer).empty());
}

TEST_CASE("app/shelf_labels: the labels follow the room and a moved module", "[app][shelf_labels]") {
    const auto& entry = planCase("plan-2000-seed-102938");
    const rv::world::CollectionLayout layout = layoutOf(entry, entry.plan);
    const RoomShelfTexts texts = textsOf(layout);
    const ShelfLabels home = rv::app::roomShelfLabels(entry.plan, entry.room, entry.data, layout, {}, texts);
    // The room placed elsewhere on its floor (the south row: turned by a half turn).
    const rv::world::RoomPlacement placement{12000, 9000, 1};
    const ShelfLabels moved = rv::app::roomShelfLabels(entry.plan, entry.room, entry.data,
                                                       layoutOf(entry, entry.plan, placement), placement, texts);
    REQUIRE(moved.tabs.size() == home.tabs.size());
    const glm::mat4 toFloor = rv::world::roomToFloorMatrix(placement);
    for (std::size_t i = 0; i < home.tabs.size(); ++i) {
        CHECK(moved.tabs[i].text == home.tabs[i].text);
        CHECK(near(origin(moved.tabs[i].frame), glm::vec3(toFloor * glm::vec4(origin(home.tabs[i].frame), 1.0f)),
                   1.0e-3f));
    }
    // A module moved along its wall by 5 cm (the editor's move): its tabs move with it, their texts the same.
    rv::world::WorldPlan edited = entry.plan;
    const rv::world::FixtureId module = home.tabs.front().fixture;
    const auto fixture = std::find_if(edited.fixtures.begin(), edited.fixtures.end(),
                                      [&](const rv::world::Fixture& candidate) { return candidate.id == module; });
    REQUIRE(fixture != edited.fixtures.end());
    const glm::vec3 along = glm::vec3(rv::world::room_frames::fixtureTransform(*fixture)[0]) * 0.05f;
    fixture->xMm += static_cast<std::int64_t>(std::lround(along.x * 1000.0f));
    fixture->zMm += static_cast<std::int64_t>(std::lround(along.z * 1000.0f));
    const ShelfLabels after =
        rv::app::roomShelfLabels(edited, entry.room, entry.data, layoutOf(entry, edited), {}, texts);
    REQUIRE(after.tabs.size() == home.tabs.size());
    for (std::size_t i = 0; i < home.tabs.size(); ++i) {
        INFO(i);
        CHECK(after.tabs[i].text == home.tabs[i].text);
        const glm::vec3 shift = origin(after.tabs[i].frame) - origin(home.tabs[i].frame);
        CHECK(near(shift, home.tabs[i].fixture == module ? along : glm::vec3(0.0f), 2.0e-3f));
    }
    CHECK(rv::app::tabTexts(after) == rv::app::tabTexts(home));
}

TEST_CASE("app/shelf_labels: a shared room's section signs", "[app][shelf_labels]") {
    const auto& entry = planCase("plan-2000-seed-102938");
    // The console room read as a shared room of two sections (the labels look at its kind and sections only).
    rv::world::WorldPlan shared = entry.plan;
    shared.rooms.front().kind = rv::world::RoomKind::Shared;
    shared.rooms.front().sections = {rv::catalog::SystemName::parse("snes").value(),
                                     rv::catalog::SystemName::parse("gb").value()};
    const rv::world::CollectionLayout layout = layoutOf(entry, shared);
    const RoomShelfTexts texts = textsOf(layout, 2);
    const ShelfLabels labels = rv::app::roomShelfLabels(shared, entry.room, entry.data, layout, {}, texts);
    REQUIRE(labels.sections.size() == 2);
    for (std::size_t s = 0; s < 2; ++s) {
        const rv::app::ShelfSectionSign& sign = labels.sections[s];
        INFO(s);
        CHECK(sign.section == s);
        CHECK(sign.system == shared.rooms.front().sections[s]);
        CHECK(sign.name == "Fixture System " + std::to_string(s));
        CHECK(sign.size.x <= rv::app::kSectionSignSize.x);
        CHECK(sign.size.y == rv::app::kSectionSignSize.y);
        // Over the fixture of the section's first item by sort key, standing on its top.
        const ShelfText* first = nullptr;
        const rv::world::CollectionItem* firstItem = nullptr;
        for (const rv::world::CollectionItem& item : layout.items) {
            const ShelfText* text = texts.find(item.game);
            const std::string& kind = prefabOf(entry, item.slot.fixture()).generator;
            if (text->section != s || (kind != "shelf" && kind != "wall_shelf" && kind != "display_case")) {
                continue;
            }
            if (first == nullptr || std::tie(text->sortKey, item.game) < std::tie(first->sortKey, firstItem->game)) {
                first = text;
                firstItem = &item;
            }
        }
        REQUIRE(firstItem != nullptr);
        const rv::world::ScenePrefab& module = prefabOf(entry, firstItem->slot.fixture());
        const glm::vec3 at = origin(sign.transform);
        CHECK(at.y == Catch::Approx(module.prefab->bounds.max.y + 0.002f + 0.5f * sign.size.y).margin(1.0e-4));
        CHECK(std::abs(glm::dot(at - origin(firstItem->slotFrame),
                                glm::vec3(1.0f, 0.0f, 1.0f) - glm::abs(glm::vec3(firstItem->slotFrame[2])))) < 0.6f);
        // Its object: a lit plate of the hall's, its generic picture its name; its id names its room and section.
        const rv::scene::SceneObject object = rv::app::sectionSignObject(labels, sign);
        CHECK(object.id == rv::app::sectionSignId(entry.room, s));
        CHECK(object.kind == "poster_frame");
        CHECK(object.texturesByRole.at("picture").pattern == "sign");
        CHECK(object.texturesByRole.at("picture").title == sign.name);
        CHECK(object.materialsByRole.at("picture") == labels.signMaterials.at("picture"));
        CHECK_FALSE(object.castsShadows);
        CHECK_FALSE(object.collides);
        CHECK(rv::geometry::buildPrefab(object.kind, object.params).has_value());
        const auto parsed = rv::app::sectionSignOf(object.id);
        REQUIRE(parsed.has_value());
        CHECK(parsed->first == entry.room);
        CHECK(parsed->second == s);
    }
    CHECK(labels.sections[0].transform != labels.sections[1].transform);
    CHECK_FALSE(rv::app::sectionSignOf("section:room:snes").has_value());
    CHECK_FALSE(rv::app::sectionSignOf("section:room:snes/01").has_value());
    CHECK_FALSE(rv::app::sectionSignOf("section:nowhere/1").has_value());
    CHECK_FALSE(rv::app::sectionSignOf("plaque:room:snes").has_value());
}

TEST_CASE("app/shelf_labels: the scene objects", "[app][shelf_labels]") {
    const auto& entry = planCase("plan-2000-seed-102938");
    const rv::world::CollectionLayout layout = layoutOf(entry, entry.plan);
    const ShelfLabels labels =
        rv::app::roomShelfLabels(entry.plan, entry.room, entry.data, layout, {}, textsOf(layout));
    const auto tabs = rv::app::shelfTabsObject(labels);
    const auto captions = rv::app::shelfCaptionsObject(labels);
    REQUIRE(tabs.has_value());
    REQUIRE(captions.has_value());
    CHECK(tabs->id == "tabs:" + entry.room.toString() + "/plates");
    CHECK(captions->id == "captions:" + entry.room.toString() + "/plates");
    // A room's prefix holds its own ids alone (room:snes's is no prefix of room:snes:2's).
    CHECK(tabs->id.starts_with(rv::app::labelsPrefix(rv::app::kTabsPrefix, entry.room.toString())));
    CHECK(rv::app::labelsPrefix(rv::app::kTabsPrefix, "room:snes") == "tabs:room:snes/");
    CHECK_FALSE(
        std::string("tabs:room:snes:2/plates").starts_with(rv::app::labelsPrefix(rv::app::kTabsPrefix, "room:snes")));
    for (const rv::scene::SceneObject* object : {&*tabs, &*captions}) {
        INFO(object->id);
        CHECK(object->kind == "label_plates");
        CHECK(object->texturesByRole.at("picture").kind == rv::scene::TextureSource::Kind::Pattern);
        CHECK_FALSE(object->castsShadows);
        CHECK_FALSE(object->collides);
        const auto built = rv::geometry::buildPrefab(object->kind, object->params);
        REQUIRE(built.has_value());
        CHECK(built->colliders.empty());
    }
    CHECK(tabs->params.at("plates").size() == labels.tabs.size());
    CHECK(captions->params.at("plates").size() == labels.captions.size());
    CHECK(tabs->materialsByRole == labels.tabMaterials);
    CHECK(captions->materialsByRole.at("plate") == labels.cardMaterial);
    // Each plate shows its own cell of the atlas its texts are drawn in.
    const auto atlas = rv::assets::labelAtlasLayout(labels.tabs.size(), rv::assets::kTabCell, rv::assets::kTabColumns);
    const nlohmann::json& cell = tabs->params.at("plates").back().at("cell");
    const glm::vec4 uv = atlas.uv(labels.tabs.size() - 1);
    CHECK(cell[0].get<float>() == uv.x);
    CHECK(cell[3].get<float>() == uv.w);
    CHECK(rv::app::tabTexts(labels).size() == labels.tabs.size());
    CHECK(rv::app::captionTexts(labels).size() == labels.captions.size());
    // None for no label.
    ShelfLabels none;
    none.room = entry.room;
    CHECK_FALSE(rv::app::shelfTabsObject(none).has_value());
    CHECK_FALSE(rv::app::shelfCaptionsObject(none).has_value());
    // The editor never takes them for a fixture, the plaques' rules never for a plaque.
    CHECK_FALSE(tabs->id.starts_with(entry.room.toString()));
}
