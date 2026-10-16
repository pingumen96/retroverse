// The catalog's identifiers (rv/catalog/ids.hpp): GameId's text form and range, every GameId a valid UI entry id,
// library ids and ES-DE system names validated.

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <unordered_set>

#include <rv/catalog/ids.hpp>
#include <rv/ui/quick_catalog_model.hpp>

using rv::catalog::GameId;
using rv::catalog::LibraryId;
using rv::catalog::SystemName;

TEST_CASE("catalog/ids: GameId text form round trips", "[catalog][ids]") {
    const GameId one = GameId::fromValue(1).value();
    CHECK(one.toString() == "g0000000000000001");
    const GameId max = GameId::fromValue(GameId::kMaxValue).value();
    CHECK(max.toString() == "g7fffffffffffffff");
    const GameId mixed = GameId::fromValue(0x0123'4567'89AB'CDEFull).value();
    CHECK(mixed.toString() == "g0123456789abcdef");

    for (const GameId id : {one, max, mixed}) {
        const std::string text = id.toString();
        CHECK(text.size() == GameId::kTextLength);
        // Every GameId names a UI entry ("game-<id>" in the product UI's documents).
        CHECK(rv::ui::isValidEntryId(text));
        const auto parsed = GameId::parse(text);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == id);
        CHECK(parsed->value() == id.value());
    }
}

TEST_CASE("catalog/ids: GameId values and texts that are none", "[catalog][ids]") {
    CHECK_FALSE(GameId::fromValue(0).has_value());
    CHECK_FALSE(GameId::fromValue(GameId::kMaxValue + 1).has_value());
    CHECK_FALSE(GameId::fromValue(~0ull).has_value());

    const GameId none;
    CHECK_FALSE(none.isValid());
    CHECK(none.value() == 0);
    CHECK(none.toString() == "g0000000000000000");
    CHECK(GameId::fromValue(42)->isValid());

    for (const std::string_view text : {
             "g0000000000000000", // 0 is no game
             "g8000000000000000", // 64 bits
             "gffffffffffffffff",
             "G0123456789abcdef", // one spelling only
             "g0123456789ABCDEF",
             "0123456789abcdef",
             "g0123456789abcde",   // too short
             "g0123456789abcdef0", // too long
             "g0123456789abcdeg",
             "g 123456789abcdef",
             "",
         }) {
        INFO(text);
        const auto parsed = GameId::parse(text);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("catalog/ids: GameIds order by value and hash", "[catalog][ids]") {
    const GameId a = GameId::fromValue(5).value();
    const GameId b = GameId::fromValue(0x10).value();
    CHECK(a < b);
    CHECK(GameId() < a);
    CHECK(a == GameId::parse("g0000000000000005").value());
    std::unordered_set<GameId> set{a, b};
    CHECK(set.contains(GameId::fromValue(5).value()));
    CHECK_FALSE(set.contains(GameId::fromValue(6).value()));
    CHECK(std::hash<GameId>{}(a) == std::hash<GameId>{}(GameId::fromValue(5).value()));
}

TEST_CASE("catalog/ids: library ids are slugs", "[catalog][ids]") {
    CHECK(LibraryId().str() == "esde");
    CHECK(LibraryId() == LibraryId::parse("esde").value());
    for (const std::string_view valid : {"esde", "main", "esde-2", "0", "a-b-c", "abcdefghijklmnopqrstuvwxyz012345"}) {
        INFO(valid);
        CHECK(LibraryId::isValid(valid));
        const auto parsed = LibraryId::parse(valid);
        REQUIRE(parsed.has_value());
        CHECK(parsed->str() == valid);
    }
    for (const std::string_view invalid : {"", "ESDE", "Esde", "es de", "es_de", "es.de", "esd\xC3\xA9",
                                           "abcdefghijklmnopqrstuvwxyz0123456" /* 33 */, "esde/1"}) {
        INFO(invalid);
        CHECK_FALSE(LibraryId::isValid(invalid));
        const auto parsed = LibraryId::parse(invalid);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::InvalidArgument);
    }
    CHECK(LibraryId::parse("esde").value() < LibraryId::parse("main").value());
}

TEST_CASE("catalog/ids: system names as ES-DE writes them", "[catalog][ids]") {
    // DEL (U+007F) is a valid character of a Windows folder name, as of a file name in a lookup key.
    for (const std::string_view valid : {"gb", "snes", "psx", "trs-80", "sg-1000", "3do", "my games", "Caf\xC3\xA9",
                                         "a.b", ".hidden", "x_y", "a\x7F"}) {
        INFO(valid);
        CHECK(SystemName::isValid(valid));
        const auto parsed = SystemName::parse(valid);
        REQUIRE(parsed.has_value());
        CHECK(parsed->str() == valid);
        CHECK_FALSE(parsed->empty());
    }
    const std::string tooLong(256, 'a');
    for (const std::string_view invalid :
         {std::string_view(""),      std::string_view("."),    std::string_view(".."),    std::string_view("gb/gbc"),
          std::string_view("gb\\x"), std::string_view("c:"),   std::string_view("a*"),    std::string_view("a?"),
          std::string_view("a\"b"),  std::string_view("a<b"),  std::string_view("a>b"),   std::string_view("a|b"),
          std::string_view("a\tb"),  std::string_view("a\nb"), std::string_view("a\x1F"), std::string_view(" gb"),
          std::string_view("gb "),   std::string_view("gb."),  std::string_view("\xC3"),  std::string_view("a\0b", 3),
          std::string_view(tooLong)}) {
        INFO(invalid);
        CHECK_FALSE(SystemName::isValid(invalid));
        const auto parsed = SystemName::parse(invalid);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::InvalidArgument);
    }
    CHECK(SystemName::isValid(std::string(255, 'a')));
    CHECK(SystemName().empty());
    // Compared byte for byte: ES-DE's names are lowercase, and "GB" is another name.
    CHECK(SystemName::parse("gb").value() != SystemName::parse("GB").value());
    CHECK(SystemName::parse("gb").value() < SystemName::parse("gbc").value());
    CHECK(SystemName::parse("gbc").value() < SystemName::parse("psx").value());
}
