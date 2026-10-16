// Lookup keys (rv/catalog/lookup_key.hpp, ADR-0008 §4): relative paths upper-cased as the Windows file systems compare
// names, never normalized, the exact path kept beside the key; malformed paths refused.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <rv/catalog/lookup_key.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/tree_walk.hpp>

#include "support/file_system.hpp"
#include "support/temp_dir.hpp"

using rv::catalog::LookupKey;
using rv::catalog::makeLookupKey;

namespace {

LookupKey keyOf(std::string_view relativePath) {
    auto key = makeLookupKey(relativePath);
    INFO(relativePath);
    REQUIRE(key.has_value());
    return std::move(*key);
}

} // namespace

TEST_CASE("catalog/lookup_key: the path upper-cased, the exact path kept", "[catalog][lookup_key]") {
    const LookupKey key = keyOf("Collection A/Pocket Puzzle (World).gb");
    CHECK(key.key() == "COLLECTION A/POCKET PUZZLE (WORLD).GB");
    CHECK(key.relativePath() == "Collection A/Pocket Puzzle (World).gb");
    CHECK_FALSE(key.empty());

    CHECK(keyOf("Tiny Tennis (Europe).gb").key() == "TINY TENNIS (EUROPE).GB");
    CHECK(keyOf("LOUD CART (USA).GB").key() == "LOUD CART (USA).GB");
    CHECK(keyOf(".Dot Hero (World).gb").key() == ".DOT HERO (WORLD).GB");
    // Names exactly as the file system holds them: trailing dots and spaces, several dots, shell characters.
    CHECK(keyOf("Stellar II - Galactic Disk .atr").key() == "STELLAR II - GALACTIC DISK .ATR");
    CHECK(keyOf("Game.gb.").key() == "GAME.GB.");
    CHECK(keyOf("a/b/c/100% Star ^3 [!] & co; x=y.cue").key() == "A/B/C/100% STAR ^3 [!] & CO; X=Y.CUE");
    CHECK(keyOf("Deep Shock (Europe).dec.iso").key() == "DEEP SHOCK (EUROPE).DEC.ISO");
}

TEST_CASE("catalog/lookup_key: beyond ASCII, never normalized", "[catalog][lookup_key]") {
    CHECK(keyOf("Caf\xC3\xA9 Racer (Europe).gb").key() == "CAF\xC3\x89 RACER (EUROPE).GB");
    // Sharp s has no single-character upper case: it stays, as on NTFS.
    CHECK(keyOf("Gro\xC3\x9F (Germany).gb").key() == "GRO\xC3\x9F (GERMANY).GB");
    // NFC and NFD are two names on disk: two keys.
    const LookupKey composed = keyOf("Caf\xC3\xA9.gb");
    const LookupKey decomposed = keyOf("Cafe\xCC\x81.gb");
    CHECK(composed.key() == "CAF\xC3\x89.GB");
    CHECK(decomposed.key() == "CAFE\xCC\x81.GB");
    CHECK(composed.key() != decomposed.key());
    // Outside the BMP nothing changes case (the file systems map UTF-16 code units).
    CHECK(keyOf("\xF0\x9F\x8E\xAE arcade.gb").key() == "\xF0\x9F\x8E\xAE ARCADE.GB");
    CHECK(keyOf("\xF0\x90\x90\xA8 long i.gb").key() == "\xF0\x90\x90\xA8 LONG I.GB");
    // The key is the file-system casing of the whole path.
    const std::string path = "Pok\xC3\xA9mon Party Mini (NA)/x.min";
    CHECK(keyOf(path).key() == rv::core::fileSystemUpperCase(path).value());
}

TEST_CASE("catalog/lookup_key: names that differ in case share a key and stay apart", "[catalog][lookup_key]") {
    const LookupKey upper = keyOf("TINY TENNIS (EUROPE).gb");
    const LookupKey title = keyOf("Tiny Tennis (Europe).gb");
    CHECK(upper.key() == title.key());
    CHECK(upper != title);
    // Ordered by key, then relative path (ordinal: 'I' before 'i'; '.' before '/').
    CHECK(upper < title);
    std::vector<LookupKey> keys{keyOf("b.gb"), title, keyOf("a/z.gb"), upper, keyOf("A.gb")};
    std::sort(keys.begin(), keys.end());
    std::vector<std::string> order;
    for (const LookupKey& key : keys) {
        order.push_back(key.relativePath());
    }
    CHECK(order ==
          std::vector<std::string>{"A.gb", "a/z.gb", "b.gb", "TINY TENNIS (EUROPE).gb", "Tiny Tennis (Europe).gb"});
    CHECK(LookupKey().empty());
    CHECK(LookupKey() < keyOf("a.gb"));
}

TEST_CASE("catalog/lookup_key: DEL is a name character, as on NTFS and exFAT", "[catalog][lookup_key]") {
    // The file systems refuse only U+0000 to U+001F in names: a ROM named with DEL (U+007F) is a game ES-DE lists.
    const std::string name = "Game\x7FName (World).gb";
    CHECK(keyOf(name).key() == "GAME\x7FNAME (WORLD).GB");

    // Such a file as the tree walker lists it, and as a gamelist names it, has one key.
    const rv::test::TempDir dir("rv-lookup-key");
    REQUIRE(rv::test::writeFileAnyLength(dir.file("Sub/" + name), "rom").has_value());
    const auto listed = rv::core::listTree(dir.path());
    REQUIRE(listed.has_value());
    std::vector<std::string> files;
    for (const rv::core::TreeEntry& entry : *listed) {
        if (entry.kind == rv::core::TreeEntryKind::File) {
            files.push_back(entry.relativePath);
        }
    }
    REQUIRE(files == std::vector<std::string>{"Sub/" + name});
    const LookupKey walked = keyOf(files[0]);
    CHECK(walked.key() == "SUB/GAME\x7FNAME (WORLD).GB");
    const auto fromGamelist = rv::core::resolveRelativePath("./sub/" + name, dir.path());
    REQUIRE(fromGamelist.has_value());
    const LookupKey written = keyOf(*fromGamelist);
    CHECK(written.key() == walked.key());
    CHECK(written.relativePath() == "sub/" + name); // the exact path as written stays apart
}

TEST_CASE("catalog/lookup_key: malformed relative paths are refused", "[catalog][lookup_key]") {
    for (const std::string_view path : {
             std::string_view(""),
             std::string_view("./a.gb"),
             std::string_view("a/./b.gb"),
             std::string_view("../a.gb"),
             std::string_view("a/../b.gb"),
             std::string_view("/a.gb"),
             std::string_view("a.gb/"),
             std::string_view("a//b.gb"),
             std::string_view("a\\b.gb"),
             std::string_view("C:/a.gb"),
             std::string_view("a.gb:stream"),
             std::string_view("a\tb.gb"),
             std::string_view("a\x1F-b.gb"),
             std::string_view("a\0b.gb", 7),
             std::string_view("\xC3(.gb"),
             std::string_view("."),
             std::string_view(".."),
         }) {
        INFO(path);
        const auto key = makeLookupKey(path);
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().code == rv::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("catalog/lookup_key: keys carry the rule and the casing table", "[catalog][lookup_key]") {
    const rv::catalog::LookupKeyVersion version = rv::catalog::currentLookupKeyVersion();
    CHECK(version.rule == rv::catalog::kLookupKeyRule);
    CHECK(version.casingTable == rv::core::casingTableVersion());
    CHECK(version == rv::catalog::currentLookupKeyVersion());
    CHECK(version != rv::catalog::LookupKeyVersion{version.rule + 1, version.casingTable});
    CHECK(version != rv::catalog::LookupKeyVersion{version.rule, version.casingTable + ".1"});
}
