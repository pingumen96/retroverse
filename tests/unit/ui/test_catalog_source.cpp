// The paged catalog source of the quick catalog (rv/ui/catalog_source.hpp) and its in-memory implementation: the
// contract rv_app's adapter of the catalog's ICatalogQuery keeps too (ADR-0008 §5).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rv/ui/catalog_source.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>

using rv::ErrorCode;
using rv::ui::CatalogEntry;
using rv::ui::CatalogSourceQuery;
using rv::ui::InMemoryCatalogSource;
using rv::ui::isValidEntryId;
using rv::ui::PlatformFilter;

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

std::vector<CatalogEntry> sample() {
    return {entry("g01", "Lanterna Cosmica", "16 bit", 1992, "Studio Aurora"),
            entry("g02", "Corsa sul Lungomare", "8 bit", 1988),
            entry("g03", "Il Faro di Vetro", "16 bit", 1994, "Officina Lunare"),
            entry("g04", "Pixel Garden", "Arcade", 1991)};
}

/// The ids of every row of the current list, fetched in pages of `page`.
std::vector<std::string> listedIds(InMemoryCatalogSource& source, std::size_t page = 3) {
    std::vector<std::string> ids;
    std::vector<CatalogEntry> rows;
    for (std::size_t first = 0; first < source.count(); first += page) {
        REQUIRE(source.rows(first, page, rows).has_value());
        for (const CatalogEntry& row : rows) {
            ids.push_back(row.id);
        }
    }
    return ids;
}

std::vector<std::string> ids(std::initializer_list<const char*> values) {
    return {values.begin(), values.end()};
}

} // namespace

TEST_CASE("ui/catalog_source: entry ids are short ASCII names", "[ui][catalog_source]") {
    CHECK(isValidEntryId("g01"));
    CHECK(isValidEntryId("snes-0042_b"));
    CHECK(isValidEntryId("g0123456789abcdef")); // the GameId text form
    CHECK_FALSE(isValidEntryId(""));
    CHECK_FALSE(isValidEntryId("g 01"));
    CHECK_FALSE(isValidEntryId("g#1"));
    CHECK_FALSE(isValidEntryId("caff\xc3\xa8"));
    CHECK_FALSE(isValidEntryId(std::string(65, 'a')));
}

TEST_CASE("ui/catalog_source: invalid and repeated ids are left out, the first entry of an id kept",
          "[ui][catalog_source]") {
    InMemoryCatalogSource source({entry("g01", "A", "8 bit", 1990), entry("g01", "B", "8 bit", 1991),
                                  entry("bad id", "C", "8 bit", 1992), entry("g02", "D", "8 bit", 1993),
                                  entry("g01", "E", "8 bit", 1994)});
    CHECK(source.entries().size() == 2);
    CHECK(source.rejected() == 3);
    CHECK(source.count() == 2);
    CHECK(listedIds(source) == ids({"g01", "g02"}));
    std::vector<CatalogEntry> rows;
    REQUIRE(source.rows(0, 1, rows).has_value());
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().title == "A");
}

TEST_CASE("ui/catalog_source: the blank query lists every entry in order, by page and by position",
          "[ui][catalog_source]") {
    InMemoryCatalogSource source(sample());
    REQUIRE(source.query(CatalogSourceQuery{}).has_value());
    CHECK(source.count() == 4);
    CHECK(listedIds(source, 1) == ids({"g01", "g02", "g03", "g04"}));
    CHECK(listedIds(source, 3) == ids({"g01", "g02", "g03", "g04"}));

    std::vector<CatalogEntry> rows;
    // A page cut at the end of the list; nothing from its end on.
    REQUIRE(source.rows(2, 10, rows).has_value());
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == sample()[2]); // every field of the row
    CHECK(rows[1].id == "g04");
    REQUIRE(source.rows(4, 2, rows).has_value());
    CHECK(rows.empty());
    REQUIRE(source.rows(99, 2, rows).has_value());
    CHECK(rows.empty());
    REQUIRE(source.rows(1, 0, rows).has_value());
    CHECK(rows.empty());

    CHECK(source.position("g01") == std::optional<std::size_t>(0));
    CHECK(source.position("g04") == std::optional<std::size_t>(3));
    CHECK_FALSE(source.position("g99").has_value());
    CHECK_FALSE(source.position("").has_value());
}

TEST_CASE("ui/catalog_source: platforms in the order the entries first name them", "[ui][catalog_source]") {
    InMemoryCatalogSource source(sample());
    const auto platforms = source.platforms();
    REQUIRE(platforms.has_value());
    CHECK(*platforms == std::vector<PlatformFilter>{PlatformFilter{"16 bit", "16 bit", 2},
                                                    PlatformFilter{"8 bit", "8 bit", 1},
                                                    PlatformFilter{"Arcade", "Arcade", 1}});

    REQUIRE(source.query(CatalogSourceQuery{"", "16 bit"}).has_value());
    CHECK(listedIds(source) == ids({"g01", "g03"}));
    CHECK(source.position("g03") == std::optional<std::size_t>(1));
    CHECK_FALSE(source.position("g02").has_value()); // not on that platform

    // A platform the catalog does not have: an error, and an empty list.
    const auto unknown = source.query(CatalogSourceQuery{"", "32 bit"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::InvalidArgument);
    CHECK(unknown.error().context == "32 bit");
    CHECK(source.count() == 0);
    CHECK_FALSE(source.position("g01").has_value());
}

TEST_CASE("ui/catalog_source: every search word must match a field, ignoring case", "[ui][catalog_source]") {
    InMemoryCatalogSource source(sample());
    const auto search = [&source](std::string text, std::string platform = {}) {
        REQUIRE(source.query(CatalogSourceQuery{std::move(text), std::move(platform)}).has_value());
        return listedIds(source);
    };
    CHECK(search("  faro  ") == ids({"g03"}));
    CHECK(search("LUNARE") == ids({"g03"}));          // developer
    CHECK(search("1988") == ids({"g02"}));            // year
    CHECK(search("arcade pixel") == ids({"g04"}));    // platform and title
    CHECK(search("arcade\tPIXEL\n") == ids({"g04"})); // any ASCII white space between words
    CHECK(search("arcade lanterna").empty());
    CHECK(search("bit", "8 bit") == ids({"g02"})); // with a platform
    CHECK(search("   ").size() == 4);
    CHECK(search("ar") == ids({"g02", "g03", "g04"})); // Lungomare, Faro, Arcade: words are substrings
    // A word never spans two fields: "garden" + "arcade" are two fields of g04, "gardenarcade" is no word of it.
    CHECK(search("gardenarcade").empty());
}

TEST_CASE("ui/catalog_source: typing on narrows the list it had, and a wider query searches everything again",
          "[ui][catalog_source]") {
    InMemoryCatalogSource source(sample());
    const auto search = [&source](std::string text) {
        REQUIRE(source.query(CatalogSourceQuery{std::move(text), {}}).has_value());
        return listedIds(source);
    };
    CHECK(search("a") == ids({"g01", "g02", "g03", "g04"}));
    CHECK(search("ar") == ids({"g02", "g03", "g04"}));
    CHECK(search("arc") == ids({"g04"}));
    CHECK(search("arca") == ids({"g04"}));
    CHECK(search("ar") == ids({"g02", "g03", "g04"})); // erased: wider again
    CHECK(search("arcz").empty());
    CHECK(search("arcz faro").empty()); // still narrower than an empty list
    CHECK(search("faro") == ids({"g03"}));
    CHECK(source.lastQuery() == CatalogSourceQuery{"faro", {}});
}

TEST_CASE("ui/catalog_source: replaced entries materialize the last query again", "[ui][catalog_source]") {
    InMemoryCatalogSource source(sample());
    REQUIRE(source.query(CatalogSourceQuery{"", "16 bit"}).has_value());
    CHECK(source.count() == 2);
    std::vector<CatalogEntry> changed = sample();
    changed.erase(changed.begin()); // g01 gone (an import removed its file)
    changed.push_back(entry("g05", "Volo Notturno", "16 bit", 1996));
    source.replaceEntries(std::move(changed));
    CHECK(listedIds(source) == ids({"g03", "g05"}));
    CHECK_FALSE(source.position("g01").has_value());
    CHECK(source.position("g05") == std::optional<std::size_t>(1));
    // A catalog without the platform lists nothing for it.
    source.replaceEntries({entry("g07", "Solo", "Arcade", 2000)});
    CHECK(source.count() == 0);
    REQUIRE(source.query(CatalogSourceQuery{}).has_value());
    CHECK(listedIds(source) == ids({"g07"}));
}

TEST_CASE("ui/catalog_source: 100,000 entries page and find in place", "[ui][catalog_source]") {
    constexpr std::size_t kGames = 100000;
    std::vector<CatalogEntry> games;
    games.reserve(kGames);
    for (std::size_t index = 0; index < kGames; ++index) {
        games.push_back(entry("g" + std::to_string(1000000 + index), "Gioco " + std::to_string(index),
                              index % 3 == 0 ? "Arcade" : "16 bit", 1980 + static_cast<int>(index % 25)));
    }
    InMemoryCatalogSource source(std::move(games));
    CHECK(source.count() == kGames);
    CHECK(source.position("g1050000") == std::optional<std::size_t>(50000));
    CHECK(source.position("g1099999") == std::optional<std::size_t>(99999));
    std::vector<CatalogEntry> rows;
    REQUIRE(source.rows(49998, 5, rows).has_value());
    REQUIRE(rows.size() == 5);
    CHECK(rows.front().title == "Gioco 49998");
    CHECK(rows.back().title == "Gioco 50002");

    REQUIRE(source.query(CatalogSourceQuery{"", "Arcade"}).has_value());
    CHECK(source.count() == 33334);
    CHECK(source.position("g1050001") == std::optional<std::size_t>(16667)); // 50001 / 3
    CHECK_FALSE(source.position("g1050000").has_value());
    REQUIRE(source.query(CatalogSourceQuery{"gioco 49998", ""}).has_value());
    CHECK(source.count() == 1);
    CHECK(source.position("g1049998") == std::optional<std::size_t>(0));
}
