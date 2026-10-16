// The quick catalog's view model, a window over a CatalogSource: selection, scrolling, pages, re-queries and the
// restore of the selection, over catalogs of a few games and of 100,000.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rv/ui/catalog_source.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/ui/quick_catalog_model.hpp>

using rv::ErrorCode;
using rv::Expected;
using rv::Status;
using rv::ui::CatalogEntry;
using rv::ui::CatalogSource;
using rv::ui::CatalogSourceQuery;
using rv::ui::InMemoryCatalogSource;
using rv::ui::PlatformFilter;
using rv::ui::QuickCatalogModel;

namespace {

constexpr std::size_t kGames = 100000;

CatalogEntry entry(std::string id, std::string title, std::string platform, int year = 1990) {
    CatalogEntry made;
    made.id = std::move(id);
    made.title = std::move(title);
    made.platform = std::move(platform);
    made.year = year;
    made.developer = "Studio";
    return made;
}

/// The id of game `index` of a generated catalog: "g" and the index after 1,000,000 (ids sort as the games do).
std::string gameId(std::size_t index) {
    return "g" + std::to_string(1000000 + index);
}

/// `count` games "Gioco <index>", every third on "Arcade" and the others on "16 bit".
std::vector<CatalogEntry> catalog(std::size_t count) {
    std::vector<CatalogEntry> games;
    games.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        games.push_back(entry(gameId(index), "Gioco " + std::to_string(index), index % 3 == 0 ? "Arcade" : "16 bit"));
    }
    return games;
}

std::vector<std::string> windowIds(const QuickCatalogModel& model) {
    std::vector<std::string> ids;
    for (const CatalogEntry& row : model.window()) {
        ids.push_back(row.id);
    }
    return ids;
}

/// The model's state in one value: the selected position, the window's first position, the window's size.
struct Placement {
    std::optional<std::size_t> selected;
    std::size_t first = 0;
    std::size_t rows = 0;

    bool operator==(const Placement&) const = default;
};

Placement placement(const QuickCatalogModel& model) {
    return {model.selectedPosition(), model.windowFirst(), model.window().size()};
}

Placement at(std::size_t selected, std::size_t first, std::size_t rows) {
    return {selected, first, rows};
}

/// A source that fails on demand (a database that cannot be read), over an in-memory one.
class FailingSource final : public CatalogSource {
public:
    explicit FailingSource(std::vector<CatalogEntry> entries) : m_inner(std::move(entries)) {}

    bool failQueries = false;
    bool failRows = false;

    Expected<std::vector<PlatformFilter>> platforms() override { return m_inner.platforms(); }
    Status query(const CatalogSourceQuery& query) override {
        if (failQueries) {
            m_failed = true;
            return rv::makeError(ErrorCode::Unavailable, "the catalog cannot be read", "test");
        }
        m_failed = false;
        return m_inner.query(query);
    }
    std::size_t count() const noexcept override { return m_failed ? 0 : m_inner.count(); }
    Status rows(std::size_t first, std::size_t count, std::vector<CatalogEntry>& out) override {
        if (failRows) {
            out.clear();
            return rv::makeError(ErrorCode::IoError, "a page cannot be read", "test");
        }
        return m_inner.rows(first, count, out);
    }
    std::optional<std::size_t> position(std::string_view id) const override {
        return m_failed ? std::nullopt : m_inner.position(id);
    }

private:
    InMemoryCatalogSource m_inner;
    bool m_failed = false;
};

} // namespace

TEST_CASE("ui/quick_catalog_model: a window of rows over 100,000 games, the first selected",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(kGames));
    QuickCatalogModel model(source, 7);
    CHECK(model.count() == kGames);
    CHECK(model.windowRows() == 7);
    CHECK(model.scrollMargin() == 2);
    CHECK(placement(model) == at(0, 0, 7));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(0));
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selected()->title == "Gioco 0");
    CHECK(windowIds(model) ==
          std::vector<std::string>{gameId(0), gameId(1), gameId(2), gameId(3), gameId(4), gameId(5), gameId(6)});
    CHECK_FALSE(model.error().has_value());
}

TEST_CASE("ui/quick_catalog_model: moves scroll the window only past the margin they move towards",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(kGames));
    QuickCatalogModel model(source, 7);
    // Down: the window stays until the selection would come closer than 2 rows to its bottom (row 4 is the last).
    for (std::size_t step = 1; step <= 4; ++step) {
        CHECK(model.moveSelection(1));
        CHECK(placement(model) == at(step, 0, 7));
    }
    CHECK(model.moveSelection(1));
    CHECK(placement(model) == at(5, 1, 7)); // scrolled by one, the selection on row 4
    CHECK(model.moveSelection(10));
    CHECK(placement(model) == at(15, 11, 7));
    // Up: the window stays until the selection would come closer than 2 rows to its top.
    CHECK(model.moveSelection(-2));
    CHECK(placement(model) == at(13, 11, 7));
    CHECK(model.moveSelection(-1));
    CHECK(placement(model) == at(12, 10, 7));
    CHECK(model.selected()->title == "Gioco 12");
    // The list's ends stop the selection; the margins give way there.
    CHECK(model.moveSelection(-1000));
    CHECK(placement(model) == at(0, 0, 7));
    CHECK_FALSE(model.moveSelection(-1));
    CHECK_FALSE(model.moveSelection(0));
    CHECK(model.moveSelection(static_cast<std::ptrdiff_t>(kGames) * 4));
    CHECK(placement(model) == at(kGames - 1, kGames - 7, 7));
    CHECK_FALSE(model.moveSelection(1));
    CHECK(model.moveSelection(-1));
    CHECK(placement(model) == at(kGames - 2, kGames - 7, 7)); // still the last window: no margin past the end
    CHECK(model.moveSelection(PTRDIFF_MIN));                  // the most negative step: no overflow
    CHECK(placement(model) == at(0, 0, 7));
}

TEST_CASE("ui/quick_catalog_model: pages move the selection and the window together, Home and End jump",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(kGames));
    QuickCatalogModel model(source, 7);
    CHECK(model.pageDown());
    CHECK(placement(model) == at(7, 7, 7)); // the selection keeps its row (the top one, from the first game)
    CHECK(model.pageDown());
    CHECK(placement(model) == at(14, 14, 7));
    CHECK(model.pageUp());
    CHECK(placement(model) == at(7, 7, 7));
    CHECK(model.end());
    CHECK(placement(model) == at(kGames - 1, kGames - 7, 7));
    CHECK_FALSE(model.end());
    CHECK_FALSE(model.pageDown());
    CHECK(model.pageUp());
    CHECK(placement(model) == at(kGames - 8, kGames - 14, 7));
    CHECK(model.pageDown());
    CHECK(placement(model) == at(kGames - 1, kGames - 7, 7));
    CHECK(model.home());
    CHECK(placement(model) == at(0, 0, 7));
    CHECK_FALSE(model.home());
    CHECK_FALSE(model.pageUp());
    // From the middle of a window, pages keep the selection's row.
    CHECK(model.moveSelection(3));
    CHECK(model.pageDown());
    CHECK(placement(model) == at(10, 7, 7));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(3));
}

TEST_CASE("ui/quick_catalog_model: a click selects a row of the window without scrolling it",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(kGames));
    QuickCatalogModel model(source, 7);
    CHECK(model.moveSelection(20));
    REQUIRE(placement(model) == at(20, 16, 7));
    CHECK(model.selectPosition(16)); // the window's top row, within the margin: no scroll
    CHECK(placement(model) == at(16, 16, 7));
    CHECK(model.selectPosition(16));
    CHECK(model.moveSelection(1)); // moving down from there does not scroll up
    CHECK(placement(model) == at(17, 16, 7));
    // A position outside the window scrolls it to the near margin.
    CHECK(model.selectPosition(50000));
    CHECK(placement(model) == at(50000, 49996, 7));
    CHECK(model.selectPosition(10));
    CHECK(placement(model) == at(10, 8, 7));
    CHECK_FALSE(model.selectPosition(kGames)); // past the end: nothing changes
    CHECK(placement(model) == at(10, 8, 7));
    // By id.
    CHECK(model.select(gameId(77777)));
    CHECK(model.selectedPosition() == std::optional<std::size_t>(77777));
    CHECK_FALSE(model.select("g9"));
    CHECK(model.selectedPosition() == std::optional<std::size_t>(77777));
}

TEST_CASE("ui/quick_catalog_model: a new search or filter keeps the selected game, else starts at its first entry",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(kGames));
    QuickCatalogModel model(source, 7);
    REQUIRE(model.select(gameId(50001)));
    REQUIRE(model.selectedSlot() == std::optional<std::size_t>(4));
    // "Arcade" holds game 50001 (every third game): it stays selected, at its place in the narrower list and on the
    // same row of the window.
    CHECK(model.setFilter("Arcade"));
    CHECK(model.count() == 33334);
    CHECK(model.selectedPosition() == std::optional<std::size_t>(16667));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(4));
    CHECK(model.selected()->id == gameId(50001));
    // A search that keeps it: the Arcade games whose title holds "5000" (15000, 45000, 50001, 50004, 50007, 75000), a
    // list shorter than the window, which shows it whole: the selection's row is its position.
    model.setSearch("gioco 5000");
    CHECK(model.count() == 6);
    CHECK(model.selected()->id == gameId(50001));
    CHECK(model.selectedPosition() == std::optional<std::size_t>(2));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(2));
    // Back to every game: still the same game, at its place in the whole list, on the row it had.
    model.setSearch("");
    CHECK(model.selectedPosition() == std::optional<std::size_t>(16667));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(2));
    CHECK(model.setFilter(""));
    CHECK(model.selectedPosition() == std::optional<std::size_t>(50001));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(2));
    // A filter without it: its first entry, the window at the top.
    CHECK(model.setFilter("16 bit")); // 50001 is an Arcade game
    CHECK(placement(model) == at(0, 0, 7));
    CHECK(model.selected()->id == gameId(1));
    // A search without games: no selection, no rows; then one game.
    model.setSearch("nessuno");
    CHECK(model.count() == 0);
    CHECK_FALSE(model.selectedPosition().has_value());
    CHECK(model.selected() == nullptr);
    CHECK(model.window().empty());
    CHECK_FALSE(model.moveSelection(1));
    CHECK_FALSE(model.pageDown());
    CHECK_FALSE(model.end());
    model.setSearch("gioco 99998");
    CHECK(placement(model) == at(0, 0, 1));
    CHECK(model.selected()->id == gameId(99998));
}

TEST_CASE("ui/quick_catalog_model: refresh keeps the selected game, else takes its nearest position",
          "[ui][quick_catalog_model]") {
    std::vector<CatalogEntry> games = catalog(1000);
    InMemoryCatalogSource source(games);
    QuickCatalogModel model(source, 7);
    REQUIRE(model.selectPosition(500));
    REQUIRE(model.selectedSlot() == std::optional<std::size_t>(4));

    // An import removed games before the selected one: the same game, its new position, on the same row.
    std::vector<CatalogEntry> fewer(games.begin() + 10, games.end());
    source.replaceEntries(fewer);
    model.refresh();
    CHECK(model.selected()->id == gameId(500));
    CHECK(model.selectedPosition() == std::optional<std::size_t>(490));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(4));

    // The selected game itself removed: the game now at its position (the next one), on the same row.
    fewer.erase(fewer.begin() + 490);
    source.replaceEntries(fewer);
    model.refresh();
    CHECK(model.selectedPosition() == std::optional<std::size_t>(490));
    CHECK(model.selected()->id == gameId(501));
    CHECK(model.selectedSlot() == std::optional<std::size_t>(4));

    // The last game selected and removed: the new last one.
    REQUIRE(model.end());
    fewer.pop_back();
    source.replaceEntries(fewer);
    model.refresh();
    CHECK(model.selectedPosition() == std::optional<std::size_t>(fewer.size() - 1));
    CHECK(model.selected()->id == fewer.back().id);
    // Fewer games than its position: the last one.
    REQUIRE(model.selectPosition(900));
    fewer.resize(100);
    source.replaceEntries(fewer);
    model.refresh();
    CHECK(model.selectedPosition() == std::optional<std::size_t>(99));
    CHECK(model.windowFirst() == 93);
    // Every game gone, then back: nothing selected, then the first.
    source.replaceEntries({});
    model.refresh();
    CHECK(model.count() == 0);
    CHECK_FALSE(model.selectedPosition().has_value());
    source.replaceEntries(games);
    model.refresh();
    CHECK(placement(model) == at(0, 0, 7));
}

TEST_CASE("ui/quick_catalog_model: refresh reads the platforms again, a filter whose platform is gone falls back",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(
        {entry("a1", "Uno", "Arcade"), entry("p1", "Due", "Portatile"), entry("a2", "Tre", "Arcade")});
    QuickCatalogModel model(source);
    REQUIRE(model.filters().size() == 3);
    CHECK(model.filters()[0] == PlatformFilter{"", "", 3}); // every game: the documents name its chip
    CHECK(model.filters()[1] == PlatformFilter{"Arcade", "Arcade", 2});
    CHECK(model.filters()[2] == PlatformFilter{"Portatile", "Portatile", 1});
    REQUIRE(model.setFilter("Portatile"));
    const std::uint64_t filterRevision = model.filterRevision();
    source.replaceEntries({entry("a1", "Uno", "Arcade"), entry("a2", "Tre", "Arcade")});
    model.refresh();
    CHECK(model.filters().size() == 2);
    CHECK(model.activeFilter().empty());
    CHECK(model.filterRevision() > filterRevision);
    CHECK(model.count() == 2);
}

TEST_CASE("ui/quick_catalog_model: the shoulders cycle the filters, wrapping around", "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(30));
    QuickCatalogModel model(source);
    REQUIRE(model.filters().size() == 3); // every game, Arcade, 16 bit
    CHECK(model.cycleFilter(1));
    CHECK(model.activeFilter() == "Arcade");
    CHECK(model.cycleFilter(1));
    CHECK(model.activeFilter() == "16 bit");
    CHECK(model.cycleFilter(1));
    CHECK(model.activeFilter().empty());
    CHECK(model.cycleFilter(-1));
    CHECK(model.activeFilter() == "16 bit");
    CHECK(model.cycleFilter(-4)); // a full turn and one more back
    CHECK(model.activeFilter() == "Arcade");
    CHECK_FALSE(model.cycleFilter(0));
    CHECK_FALSE(model.cycleFilter(3)); // a full turn: the same filter
    CHECK_FALSE(model.setFilter("32 bit"));
    CHECK(model.activeFilter() == "Arcade");
    InMemoryCatalogSource single({entry("x1", "Solo", "")});
    QuickCatalogModel lone(single);
    CHECK(lone.filters().size() == 1);
    CHECK_FALSE(lone.cycleFilter(1));
}

TEST_CASE("ui/quick_catalog_model: another window size keeps the selection's row where it can",
          "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(kGames));
    QuickCatalogModel model(source, 7);
    REQUIRE(model.selectPosition(50000));
    REQUIRE(model.selectedSlot() == std::optional<std::size_t>(4));
    model.setWindowRows(5); // fewer rows (a smaller view, a larger text): row 4 is in the bottom margin now
    CHECK(model.window().size() == 5);
    CHECK(model.selectedSlot() == std::optional<std::size_t>(2));
    model.setWindowRows(12);
    CHECK(model.window().size() == 12);
    CHECK(model.selectedSlot() == std::optional<std::size_t>(2));
    CHECK(model.selectedPosition() == std::optional<std::size_t>(50000));
    model.setWindowRows(0); // at least one row
    CHECK(model.windowRows() == 1);
    CHECK(model.scrollMargin() == 0);
    CHECK(placement(model) == at(50000, 50000, 1));
    // At the start of the list the window stays at its top.
    REQUIRE(model.home());
    model.setWindowRows(7);
    CHECK(placement(model) == at(0, 0, 7));
    REQUIRE(model.moveSelection(1));
    model.setWindowRows(5);
    model.setWindowRows(7);
    CHECK(placement(model) == at(1, 0, 7)); // and back: the same placement
}

TEST_CASE("ui/quick_catalog_model: a list shorter than the window shows it whole", "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(4));
    QuickCatalogModel model(source, 7);
    CHECK(placement(model) == at(0, 0, 4));
    CHECK(model.pageDown());
    CHECK(placement(model) == at(3, 0, 4));
    CHECK(model.pageUp());
    CHECK(placement(model) == at(0, 0, 4));
    CHECK(model.end());
    CHECK(placement(model) == at(3, 0, 4));
}

TEST_CASE("ui/quick_catalog_model: revisions count real changes only", "[ui][quick_catalog_model]") {
    InMemoryCatalogSource source(catalog(100));
    QuickCatalogModel model(source, 7);
    const std::uint64_t list = model.listRevision();
    const std::uint64_t window = model.windowRevision();
    const std::uint64_t selection = model.selectionRevision();
    const std::uint64_t filter = model.filterRevision();
    CHECK(model.selectPosition(0)); // already selected
    model.setSearch("");            // unchanged
    CHECK(model.setFilter(""));     // unchanged
    model.setWindowRows(7);         // unchanged
    CHECK(model.listRevision() == list);
    CHECK(model.windowRevision() == window);
    CHECK(model.selectionRevision() == selection);
    CHECK(model.filterRevision() == filter);

    CHECK(model.moveSelection(1)); // inside the window: the selection changes, the window does not
    CHECK(model.selectionRevision() == selection + 1);
    CHECK(model.windowRevision() == window);
    CHECK(model.moveSelection(5)); // past the margin: both
    CHECK(model.selectionRevision() == selection + 2);
    CHECK(model.windowRevision() == window + 1);
    CHECK(model.setFilter("Arcade")); // the filter and the list; game 6 is an Arcade game: the same selection
    CHECK(model.filterRevision() == filter + 1);
    CHECK(model.listRevision() == list + 1);
    CHECK(model.selectionRevision() == selection + 2);
    model.setSearch("gioco 6"); // another list, the same game selected
    CHECK(model.listRevision() == list + 2);
    CHECK(model.selectionRevision() == selection + 2);
    CHECK(model.selected()->id == gameId(6));
}

TEST_CASE("ui/quick_catalog_model: errors of the source empty the list until a query succeeds",
          "[ui][quick_catalog_model]") {
    FailingSource source(catalog(50));
    QuickCatalogModel model(source, 7);
    REQUIRE(model.count() == 50);
    REQUIRE(model.moveSelection(10));

    source.failQueries = true;
    model.setSearch("gioco");
    REQUIRE(model.error().has_value());
    CHECK(model.error()->code == ErrorCode::Unavailable);
    CHECK(model.count() == 0);
    CHECK(model.selected() == nullptr);
    CHECK(model.window().empty());

    source.failQueries = false;
    model.setSearch("gioco 4"); // 4, 14, 24, 34, 40 to 49 (no year holds a 4)
    CHECK_FALSE(model.error().has_value());
    CHECK(model.count() == 14);

    source.failRows = true;
    CHECK(model.end());
    REQUIRE(model.error().has_value());
    CHECK(model.error()->code == ErrorCode::IoError);
    CHECK(model.window().empty());
    CHECK(model.selected() == nullptr);
    source.failRows = false;
    CHECK(model.home());
    CHECK_FALSE(model.error().has_value());
    CHECK(model.selected()->id == gameId(4));
}
