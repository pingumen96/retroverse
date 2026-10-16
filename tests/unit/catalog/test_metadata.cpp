// The metadata normalizers (rv/catalog/metadata.hpp) on the formats ES-DE writes, as the survey of the reference
// library found them (date-times "19970702T000000", year-only dates, ratings "0.1" to "1", players "1-2", "8+",
// "2-3000", playtime in seconds; trailing spaces, line feeds, double spaces in text), with invented names; the display,
// title and sort key rules.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/metadata.hpp>

using namespace rv::catalog;

TEST_CASE("catalog/metadata: ES-DE date-times", "[catalog][metadata]") {
    // Last played: local time, without a zone.
    CHECK(parseEsdeDateTime("20250712T104023").value() == LocalDateTime{2025, 7, 12, 10, 40, 23});
    CHECK(parseEsdeDateTime("20260906T190454").value() == LocalDateTime{2026, 9, 6, 19, 4, 54});
    CHECK(parseEsdeDateTime(" 19970702T000000\n").value() == LocalDateTime{1997, 7, 2, 0, 0, 0});
    CHECK(parseEsdeDateTime("20000229T235959").value() == LocalDateTime{2000, 2, 29, 23, 59, 59}); // leap year
    for (const std::string_view text :
         {"19990230T000000", "19000229T000000", "20251301T000000", "20250700T000000", "20250712T244023",
          "20250712T106023", "20250712T104060", "00000101T000000", "2025-07-12T10:40:23", "20250712 104023",
          "20250712t104023", "20250712T10402", "202507120T104023", "not-a-date-time", "", "+0250712T104023"}) {
        INFO(text);
        const auto time = parseEsdeDateTime(text);
        REQUIRE_FALSE(time.has_value());
        CHECK(time.error().code == rv::ErrorCode::ParseError);
    }
    for (const std::string_view text : {"20250712T104023", "19700101T000000", "00010101T000000", "99991231T235959"}) {
        CHECK(formatEsdeDateTime(parseEsdeDateTime(text).value()) == text);
    }
    CHECK(LocalDateTime{2025, 7, 12, 10, 40, 23} < LocalDateTime{2025, 7, 12, 10, 40, 24});
}

TEST_CASE("catalog/metadata: release dates", "[catalog][metadata]") {
    CHECK(parseReleaseDate("19970702T000000").value() == ReleaseDate{1997, 7, 2});
    // Year precision as scrapers write it: 1 January, which cannot be told from a real 1 January.
    CHECK(parseReleaseDate("19900101T000000").value() == ReleaseDate{1990, 1, 1});
    CHECK(parseReleaseDate("19761231T120000").value() == ReleaseDate{1976, 12, 31});
    CHECK_FALSE(parseReleaseDate("1990").has_value());
    CHECK_FALSE(parseReleaseDate("19900101").has_value());
    CHECK(ReleaseDate{1990, 1, 1} < ReleaseDate{1990, 1, 2});
}

TEST_CASE("catalog/metadata: ratings in tenths", "[catalog][metadata]") {
    CHECK(parseRating("0").value() == Rating{0});
    CHECK(parseRating("1").value() == Rating{10});
    for (int tenths = 1; tenths <= 9; ++tenths) {
        const std::string text = "0." + std::to_string(tenths);
        INFO(text);
        CHECK(parseRating(text).value() == Rating{tenths});
        CHECK(formatRating(Rating{tenths}) == text);
    }
    CHECK(parseRating("1.0").value() == Rating{10});
    CHECK(parseRating("1.00").value() == Rating{10});
    CHECK(parseRating(" 0.5\n").value() == Rating{5});
    // Two decimals (other tools): the nearest tenth, halves up.
    CHECK(parseRating("0.75").value() == Rating{8});
    CHECK(parseRating("0.749").value() == Rating{7});
    CHECK(parseRating("0.05").value() == Rating{1});
    CHECK(parseRating("0.95").value() == Rating{10});
    CHECK(formatRating(Rating{0}) == "0");
    CHECK(formatRating(Rating{10}) == "1");
    for (const std::string_view text :
         {"", "-0.5", "+0.5", "1.5", "1.01", "2", "0.", ".5", "0,5", "1e-1", "0.5x", "00.5", "0 .5", "unknown"}) {
        INFO(text);
        const auto rating = parseRating(text);
        REQUIRE_FALSE(rating.has_value());
        CHECK(rating.error().code == rv::ErrorCode::ParseError);
    }
}

TEST_CASE("catalog/metadata: players", "[catalog][metadata]") {
    CHECK(parsePlayers("1").value() == Players{1, 1});
    CHECK(parsePlayers("1-2").value() == Players{1, 2});
    CHECK(parsePlayers("1-4").value() == Players{1, 4});
    CHECK(parsePlayers("2-16").value() == Players{2, 16});
    CHECK(parsePlayers("24").value() == Players{24, 24});
    CHECK(parsePlayers("2-3000").value() == Players{2, 3000});
    CHECK(parsePlayers("8+").value() == Players{8, std::nullopt});
    CHECK(parsePlayers(" 1-2\n").value() == Players{1, 2});
    CHECK(parsePlayers("2-2").value() == Players{2, 2});
    CHECK(parsePlayers("4294967295").value() == Players{4294967295u, 4294967295u});
    for (const std::string_view text : {"1", "1-2", "2-16", "24", "2-3000", "8+"}) {
        CHECK(formatPlayers(parsePlayers(text).value()) == text);
    }
    CHECK(formatPlayers(Players{2, 2}) == "2");
    for (const std::string_view text : {"", "0", "0-2", "2-1", "1-", "-2", "+", "1+2", "1-2+", "1 - 2", "1, 2",
                                        "unknown", "4294967296", "1-4294967296", "1.5"}) {
        INFO(text);
        const auto players = parsePlayers(text);
        REQUIRE_FALSE(players.has_value());
        CHECK(players.error().code == rv::ErrorCode::ParseError);
    }
}

TEST_CASE("catalog/metadata: ES-DE booleans and counts", "[catalog][metadata]") {
    CHECK(parseEsdeBool("true").value());
    CHECK_FALSE(parseEsdeBool("false").value());
    // Exactly as ES-DE writes them: ES-DE itself takes anything but "true" as false.
    for (const std::string_view text : {"TRUE", "True", " true", "true\n", "1", "yes", ""}) {
        INFO(text);
        CHECK_FALSE(parseEsdeBool(text).has_value());
    }

    CHECK(parseEsdeCount("0").value() == 0);
    CHECK(parseEsdeCount("363").value() == 363); // playtime in seconds
    CHECK(parseEsdeCount("33552").value() == 33552);
    CHECK(parseEsdeCount(" 12\n").value() == 12);
    CHECK(parseEsdeCount("9223372036854775807").value() == 9223372036854775807ull);
    for (const std::string_view text :
         {"", "-1", "+1", "1.5", "12abc", "1 2", "9223372036854775808", "99999999999999999999999"}) {
        INFO(text);
        const auto count = parseEsdeCount(text);
        REQUIRE_FALSE(count.has_value());
        CHECK(count.error().code == rv::ErrorCode::ParseError);
    }
}

TEST_CASE("catalog/metadata: text shown on one line", "[catalog][metadata]") {
    CHECK(displayText("Mirage Soft\n") == "Mirage Soft");
    CHECK(displayText("Mirage Soft \n") == "Mirage Soft");
    CHECK(displayText("Lantern Works ") == "Lantern Works");
    CHECK(displayText("  Tiny Tennis") == "Tiny Tennis");
    // Spaces inside are the text's own; line breaks and tabs become one space.
    CHECK(displayText("10 out of 10 :  Volcanoes") == "10 out of 10 :  Volcanoes");
    CHECK(displayText("Line one\nLine two") == "Line one Line two");
    CHECK(displayText("A\r\n\r\nB") == "A B");
    CHECK(displayText("Tab\tbetween") == "Tab between");
    CHECK(displayText("A \n B") == "A B");
    CHECK(displayText("Corsa, Guida / Moto") == "Corsa, Guida / Moto");
    CHECK(displayText("Caf\xC3\xA9") == "Caf\xC3\xA9");
    CHECK(displayText(" \n\t ") == "");
    CHECK(displayText("") == "");
}

TEST_CASE("catalog/metadata: titles of files without metadata", "[catalog][metadata]") {
    // The stem as ES-DE shows it: only the last extension goes, tags and brackets stay.
    CHECK(titleFromFileStem("Pocket Puzzle (USA, Europe) (SGB Enhanced).gb") ==
          "Pocket Puzzle (USA, Europe) (SGB Enhanced)");
    CHECK(titleFromFileStem("1 GB US - H-Q/Pocket Puzzle (World) [!].gb") == "Pocket Puzzle (World) [!]");
    CHECK(titleFromFileStem("Deep Shock (Europe) (En,Fr,De,Es,It).dec.iso") ==
          "Deep Shock (Europe) (En,Fr,De,Es,It).dec");
    CHECK(titleFromFileStem("Z/Zero-Out_Disk1.zip") == "Zero-Out_Disk1");
    // Trimmed: a space before the extension goes.
    CHECK(titleFromFileStem("Galactic Disk .atr") == "Galactic Disk");
    // A leading dot stays; no extension keeps the name; a name that is only an extension is kept whole.
    CHECK(titleFromFileStem(".Dot Hero (World).gb") == ".Dot Hero (World)");
    CHECK(titleFromFileStem("witching hour") == "witching hour");
    CHECK(titleFromFileStem("games/.gb") == ".gb");
    CHECK(titleFromFileStem("Caf\xC3\xA9 Racer (Europe).gb") == "Caf\xC3\xA9 Racer (Europe)");
    // Folders keep their whole name.
    CHECK(titleFromFileStem("dos/QUEST SIX", true) == "QUEST SIX");
    CHECK(titleFromFileStem("Vol. 2", true) == "Vol. 2");
    CHECK(titleFromFileStem("Disc Game.cue", true) == "Disc Game.cue");

    GameMetadata metadata;
    CHECK(defaultTitle(metadata, "Collection B/Pocket Puzzle (World).gb") == "Pocket Puzzle (World)");
    metadata.name = " Pocket Puzzle \n";
    CHECK(defaultTitle(metadata, "Collection B/Pocket Puzzle (World).gb") == "Pocket Puzzle");
    metadata.name = " \n";
    CHECK(defaultTitle(metadata, "Collection A", true) == "Collection A");
}

TEST_CASE("catalog/metadata: sort keys (version 1)", "[catalog][metadata]") {
    const auto key = [](std::string_view title) {
        return sortKeyOf(title).value();
    };
    CHECK(kSortKeyVersion == 1);
    CHECK(key("Tiny Tennis") == "tiny tennis");
    // White space trimmed and collapsed; leading articles stay where they are.
    CHECK(key("  The  Ninja\n Returns ") == "the ninja returns");
    CHECK(key("10 out of 10 :  Volcanoes") == "10 out of 10 : volcanoes");
    CHECK(key("") == "");
    // Lower case, so '_' (0x5F) sorts before letters; upper-casing would have put it after them.
    CHECK(key("_Under") < key("Alpha"));
    // Digits compare as text.
    CHECK(key("10 Pin") < key("2 Pin"));
    // Accented letters are not folded: they sort by code point, after z.
    CHECK(key("CAF\xC3\x89") == "caf\xC3\xA9");
    CHECK(key("Cafe") < key("Cafz"));
    CHECK(key("Cafz") < key("Caf\xC3\xA9"));
    // Case never matters.
    CHECK(key("POCKET PUZZLE") == key("pocket puzzle"));

    std::vector<std::string> titles{"beta Run", "Alpha Quest", "alpha quest", "The Ninja", "2 Pin", "10 Pin"};
    std::vector<std::string> keys;
    for (const std::string& title : titles) {
        keys.push_back(key(title));
    }
    std::sort(keys.begin(), keys.end());
    CHECK(keys == std::vector<std::string>{"10 pin", "2 pin", "alpha quest", "alpha quest", "beta run", "the ninja"});

    const auto malformed = sortKeyOf("\xC3");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("catalog/metadata: the gamelist elements the catalog models", "[catalog][metadata]") {
    for (const std::string_view name :
         {"path", "name", "sortname", "desc", "developer", "publisher", "genre", "altemulator"}) {
        INFO(name);
        CHECK(isModelledGamelistElement(name));
        CHECK_FALSE(isNormalizedGamelistElement(name));
    }
    for (const std::string_view name : {"releasedate", "rating", "players", "favorite", "hidden", "kidgame",
                                        "completed", "broken", "playcount", "playtime", "lastplayed"}) {
        INFO(name);
        CHECK(isModelledGamelistElement(name));
        CHECK(isNormalizedGamelistElement(name));
    }
    // ES-DE's fields that the catalog does not use, and other tools' elements, are unknown: kept raw.
    for (const std::string_view name : {"controller", "collectionsortname", "nogamecount", "nomultiscrape",
                                        "hidemetadata", "region", "md5", "image", "Name", ""}) {
        INFO(name);
        CHECK_FALSE(isModelledGamelistElement(name));
        CHECK_FALSE(isNormalizedGamelistElement(name));
    }

    RawEsdeValues raw;
    CHECK(raw.empty());
    raw.originalForms = {{"rating", "0.70"}, {"players", " 1-2"}};
    raw.unknownElements = {{"region", "wor"}};
    REQUIRE(raw.originalForm("rating") != nullptr);
    CHECK(*raw.originalForm("rating") == "0.70");
    CHECK(*raw.originalForm("players") == " 1-2");
    CHECK(raw.originalForm("releasedate") == nullptr);
    CHECK_FALSE(raw.empty());
}
