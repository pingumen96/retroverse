#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rv/ui/quick_catalog_model.hpp>

using rv::ui::CatalogEntry;
using rv::ui::isValidEntryId;
using rv::ui::QuickCatalogModel;

namespace {

CatalogEntry entry(std::string id, std::string title, std::string platform, int year,
                   std::string developer = "Studio") {
    CatalogEntry made;
    made.id = std::move(id);
    made.title = std::move(title);
    made.platform = std::move(platform);
    made.year = year;
    made.developer = std::move(developer);
    made.genre = "Azione";
    return made;
}

std::vector<std::string> visibleIds(const QuickCatalogModel& model) {
    std::vector<std::string> ids;
    for (const std::size_t index : model.visible()) {
        ids.push_back(model.entries()[index].id);
    }
    return ids;
}

QuickCatalogModel sample() {
    return QuickCatalogModel({entry("g01", "Lanterna Cosmica", "16 bit", 1992, "Studio Aurora"),
                              entry("g02", "Corsa sul Lungomare", "8 bit", 1988),
                              entry("g03", "Il Faro di Vetro", "16 bit", 1994, "Officina Lunare"),
                              entry("g04", "Pixel Garden", "Arcade", 1991)});
}

} // namespace

TEST_CASE("ui/quick_catalog_model: entry ids are short ASCII names", "[ui][quick_catalog_model]") {
    CHECK(isValidEntryId("g01"));
    CHECK(isValidEntryId("snes-0042_b"));
    CHECK_FALSE(isValidEntryId(""));
    CHECK_FALSE(isValidEntryId("g 01"));
    CHECK_FALSE(isValidEntryId("g#1"));
    CHECK_FALSE(isValidEntryId("caff\xc3\xa8"));
    CHECK_FALSE(isValidEntryId(std::string(65, 'a')));
}

TEST_CASE("ui/quick_catalog_model: invalid and duplicate ids are left out", "[ui][quick_catalog_model]") {
    const QuickCatalogModel model({entry("g01", "A", "8 bit", 1990), entry("g01", "B", "8 bit", 1991),
                                   entry("bad id", "C", "8 bit", 1992), entry("g02", "D", "8 bit", 1993)});
    CHECK(model.entries().size() == 2);
    CHECK(model.rejected() == 2);
    CHECK(model.find("g02") != nullptr);
    CHECK(model.find("bad id") == nullptr);
}

TEST_CASE("ui/quick_catalog_model: filters list every game, then each platform in catalog order",
          "[ui][quick_catalog_model]") {
    QuickCatalogModel model = sample();
    REQUIRE(model.filters().size() == 4);
    CHECK(model.filters()[0].key.empty());
    CHECK(model.filters()[0].label == "Tutte");
    CHECK(model.filters()[0].count == 4);
    CHECK(model.filters()[1].key == "16 bit");
    CHECK(model.filters()[1].count == 2);
    CHECK(model.filters()[2].key == "8 bit");
    CHECK(model.filters()[3].key == "Arcade");

    CHECK(visibleIds(model) == std::vector<std::string>{"g01", "g02", "g03", "g04"});
    CHECK(model.setFilter("16 bit"));
    CHECK(model.activeFilter() == "16 bit");
    CHECK(visibleIds(model) == std::vector<std::string>{"g01", "g03"});
    CHECK_FALSE(model.setFilter("32 bit")); // unknown: nothing changes
    CHECK(model.activeFilter() == "16 bit");
    CHECK(model.setFilter(""));
    CHECK(model.visible().size() == 4);
}

TEST_CASE("ui/quick_catalog_model: every search word must match a field, ignoring case", "[ui][quick_catalog_model]") {
    QuickCatalogModel model = sample();
    model.setSearch("  faro  ");
    CHECK(visibleIds(model) == std::vector<std::string>{"g03"});
    model.setSearch("LUNARE");
    CHECK(visibleIds(model) == std::vector<std::string>{"g03"}); // developer
    model.setSearch("1988");
    CHECK(visibleIds(model) == std::vector<std::string>{"g02"}); // year
    model.setSearch("arcade pixel");
    CHECK(visibleIds(model) == std::vector<std::string>{"g04"}); // platform and title
    model.setSearch("arcade lanterna");
    CHECK(model.visible().empty());
    CHECK(model.selected() == nullptr);
    model.setSearch("   ");
    CHECK(model.visible().size() == 4);
    CHECK(model.search() == "   ");
}

TEST_CASE("ui/quick_catalog_model: the selection survives what keeps it visible", "[ui][quick_catalog_model]") {
    QuickCatalogModel model = sample();
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selected()->id == "g01"); // the first visible game
    CHECK(model.select("g03"));
    CHECK(model.selected()->id == "g03");
    CHECK_FALSE(model.select("g99"));
    CHECK(model.selected()->id == "g03");

    CHECK(model.setFilter("16 bit")); // g03 stays visible
    CHECK(model.selected()->id == "g03");
    CHECK_FALSE(model.select("g02")); // filtered out: cannot be selected
    CHECK(model.setFilter("8 bit"));  // g03 is gone: the first visible game
    CHECK(model.selected()->id == "g02");
    CHECK(model.isVisible("g02"));
    CHECK_FALSE(model.isVisible("g03"));
}

TEST_CASE("ui/quick_catalog_model: revisions count real changes only", "[ui][quick_catalog_model]") {
    QuickCatalogModel model = sample();
    const std::uint64_t visible = model.visibleRevision();
    const std::uint64_t selection = model.selectionRevision();
    CHECK(model.select("g01")); // already selected
    model.setSearch("");        // unchanged
    CHECK(model.setFilter("")); // unchanged
    CHECK(model.visibleRevision() == visible);
    CHECK(model.selectionRevision() == selection);

    CHECK(model.select("g02"));
    CHECK(model.selectionRevision() == selection + 1);
    model.setSearch("corsa"); // g02 stays: the list changes, the selection does not
    CHECK(model.visibleRevision() == visible + 1);
    CHECK(model.selectionRevision() == selection + 1);
    model.setSearch("corsa "); // the same words: the same games
    CHECK(model.visibleRevision() == visible + 1);
}
