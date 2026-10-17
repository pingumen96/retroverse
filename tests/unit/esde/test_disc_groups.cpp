// Disc groups (I2 step 7): reading cue sheets and playlists (encodings, FILE lines, comments), resolving their
// references against the system folder, and grouping heads into groups without cycles.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/esde/disc_groups.hpp>

#include "support/file_system.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::catalog::DiscGroupKind;
using rv::catalog::LookupKey;

namespace {

LookupKey key(std::string_view path) {
    auto made = rv::catalog::makeLookupKey(path);
    REQUIRE(made.has_value());
    return std::move(*made);
}

rv::catalog::SystemName psx() {
    return rv::catalog::SystemName::parse("psx").value();
}

std::vector<std::string> paths(const std::vector<LookupKey>& keys) {
    std::vector<std::string> out;
    for (const LookupKey& k : keys) {
        out.push_back(k.relativePath());
    }
    return out;
}

} // namespace

TEST_CASE("esde/disc_groups: the kind of sheet a name is", "[esde][disc_groups]") {
    CHECK(rv::esde::discSheetKind("Game.cue") == DiscGroupKind::Cue);
    CHECK(rv::esde::discSheetKind("Game.CUE") == DiscGroupKind::Cue);
    CHECK(rv::esde::discSheetKind("Game.Cue") == DiscGroupKind::Cue);
    CHECK(rv::esde::discSheetKind("Game.m3u") == DiscGroupKind::M3u);
    CHECK(rv::esde::discSheetKind("Game.M3U") == DiscGroupKind::M3u);
    CHECK_FALSE(rv::esde::discSheetKind("Game.bin").has_value());
    CHECK_FALSE(rv::esde::discSheetKind("Game.m3u8").has_value());
    CHECK_FALSE(rv::esde::discSheetKind("cue").has_value());
    CHECK_FALSE(rv::esde::discSheetKind("Game.cue.zip").has_value());
}

TEST_CASE("esde/disc_groups: sheets decode as UTF-8, else Windows-1252", "[esde][disc_groups]") {
    CHECK(rv::esde::decodeSheet("FILE \"a.bin\"").text == "FILE \"a.bin\"");
    const rv::esde::SheetText bom = rv::esde::decodeSheet("\xEF\xBB\xBF" "FILE \"Caf\xC3\xA9.bin\"");
    CHECK(bom.text == "FILE \"Caf\xC3\xA9.bin\"");
    CHECK_FALSE(bom.windows1252);
    const rv::esde::SheetText ansi = rv::esde::decodeSheet("FILE \"Caf\xE9 \x80.bin\"");
    CHECK(ansi.text == "FILE \"Caf\xC3\xA9 \xE2\x82\xAC.bin\"");
    CHECK(ansi.windows1252);
}

TEST_CASE("esde/disc_groups: the FILE lines of a cue sheet", "[esde][disc_groups]") {
    const std::string sheet = "REM GENRE Racing\r\n"
                              "FILE \"Track 1.bin\" BINARY\r\n"
                              "  TRACK 01 MODE2/2352\r\n"
                              "    INDEX 01 00:00:00\r\n"
                              "\tfile \"Sub/Track 2.bin\" BINARY\n"
                              "FILE Plain.bin BINARY\n"
                              "FILE Two Words.wav WAVE\n"
                              "FILE alone.bin\n"
                              "FILE \"unclosed name.bin BINARY\n"
                              "FILES \"not a file line.bin\" BINARY\n"
                              "FILE\n"
                              "FILE \"\" BINARY\n"
                              "TITLE \"FILE inside.bin\"\n";
    CHECK(rv::esde::cueSheetFiles(sheet) ==
          std::vector<std::string>{"Track 1.bin", "Sub/Track 2.bin", "Plain.bin", "Two Words.wav", "alone.bin",
                                   "unclosed name.bin BINARY"});
    CHECK(rv::esde::cueSheetFiles("").empty());
}

TEST_CASE("esde/disc_groups: the entries of a playlist", "[esde][disc_groups]") {
    CHECK(rv::esde::m3uFiles("#EXTM3U\r\n# Disc list\r\nDisc 1.cue\r\n\r\n  Disc 2.cue  \n\t#indented comment\nsub\\Disc 3.chd") ==
          std::vector<std::string>{"Disc 1.cue", "Disc 2.cue", "sub\\Disc 3.chd"});
}

TEST_CASE("esde/disc_groups: references resolve against the sheet's folder, inside the system folder",
          "[esde][disc_groups]") {
    const fs::path folder = "C:\\Roms\\psx";
    const auto resolve = [&](std::string_view reference, std::string_view sheet) {
        return rv::esde::resolveSheetReference(reference, sheet, folder);
    };
    CHECK(resolve("Track 1.bin", "Game.cue").value() == "Track 1.bin");
    CHECK(resolve("Track 1.bin", "Sub/Game.cue").value() == "Sub/Track 1.bin");
    CHECK(resolve("..\\Tracks\\Track 1.bin", "Discs/Game.cue").value() == "Tracks/Track 1.bin");
    CHECK(resolve("./a/./b//c.bin", "Game.cue").value() == "a/b/c.bin");
    CHECK(resolve("C:\\Roms\\psx\\Other\\x.bin", "Game.cue").value() == "Other/x.bin");
    CHECK(resolve("c:/roms/PSX/x.bin", "Sub/Game.cue").value() == "x.bin");
    CHECK(resolve("..\\x.bin", "Game.cue").error().code == rv::ErrorCode::OutOfRange);
    CHECK(resolve("D:\\Elsewhere\\x.bin", "Game.cue").error().code == rv::ErrorCode::OutOfRange);
    CHECK(resolve("C:x.bin", "Game.cue").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(resolve("\\x.bin", "Game.cue").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(resolve("a:b.bin", "Game.cue").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(resolve("", "Game.cue").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(resolve("a\tb.bin", "Game.cue").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(resolve("..", "Sub/Game.cue").error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("esde/disc_groups: a sheet read from disk", "[esde][disc_groups]") {
    const rv::test::TempDir dir("rv-disc-sheet");
    REQUIRE(rv::test::writeFileAnyLength(dir.file("psx/Game.cue"), "FILE \"Game.bin\" BINARY\n"));
    REQUIRE(rv::test::writeFileAnyLength(dir.file("psx/List.m3u"), "Game.cue\n"));
    const auto cue = rv::esde::readDiscSheet(dir.file("psx"), key("Game.cue"), DiscGroupKind::Cue);
    REQUIRE(cue.has_value());
    CHECK(cue->references == std::vector<std::string>{"Game.bin"});
    const auto m3u = rv::esde::readDiscSheet(dir.file("psx"), key("List.m3u"), DiscGroupKind::M3u);
    REQUIRE(m3u.has_value());
    CHECK(m3u->references == std::vector<std::string>{"Game.cue"});
    const auto missing = rv::esde::readDiscSheet(dir.file("psx"), key("None.cue"), DiscGroupKind::Cue);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    CHECK(missing.error().context.ends_with("psx\\None.cue"));
    REQUIRE(rv::test::writeFileAnyLength(dir.file("psx/Big.cue"), std::string(rv::esde::kMaxDiscSheetBytes + 1, 'x')));
    const auto big = rv::esde::readDiscSheet(dir.file("psx"), key("Big.cue"), DiscGroupKind::Cue);
    REQUIRE_FALSE(big.has_value());
    CHECK(big.error().code == rv::ErrorCode::OutOfRange);
}

TEST_CASE("esde/disc_groups: groups, chains, shared members and cycles", "[esde][disc_groups]") {
    const fs::path folder = "C:\\Roms\\psx";
    SECTION("a cue sheet and its tracks") {
        const auto grouping =
            rv::esde::groupDiscs(psx(), {{key("A.cue"), DiscGroupKind::Cue, {key("A 1.bin"), key("A 2.bin")}}}, folder);
        REQUIRE(grouping.groups.size() == 1);
        CHECK(grouping.groups[0].head.relativePath() == "A.cue");
        CHECK(paths(grouping.groups[0].members) == std::vector<std::string>{"A 1.bin", "A 2.bin"});
        REQUIRE(grouping.headOf(key("A 2.bin")) != nullptr);
        CHECK(grouping.headOf(key("A 2.bin"))->relativePath() == "A.cue");
        CHECK(grouping.headOf(key("A.cue")) == nullptr);
        CHECK(grouping.issues.empty());
    }
    SECTION("a chain: the playlist's members first in the members-first order") {
        const auto grouping = rv::esde::groupDiscs(
            psx(),
            {{key("Saga.m3u"), DiscGroupKind::M3u, {key("Saga 1.cue"), key("Saga 2.cue")}},
             {key("Saga 2.cue"), DiscGroupKind::Cue, {key("Saga 2.bin")}},
             {key("Saga 1.cue"), DiscGroupKind::Cue, {key("Saga 1.bin")}}},
            folder);
        REQUIRE(grouping.groups.size() == 3);
        CHECK(grouping.groups[0].head.relativePath() == "Saga 1.cue"); // by key
        CHECK(grouping.groups[2].head.relativePath() == "Saga.m3u");
        CHECK(grouping.headOf(key("Saga 1.cue"))->relativePath() == "Saga.m3u");
        CHECK(grouping.headOf(key("Saga 1.bin"))->relativePath() == "Saga 1.cue");
        REQUIRE(grouping.membersFirst.size() == 3);
        CHECK(grouping.membersFirst.back() == 2); // the playlist after its cue sheets
    }
    SECTION("a member two heads reference: shown under the first") {
        const auto grouping = rv::esde::groupDiscs(psx(),
                                                   {{key("B.cue"), DiscGroupKind::Cue, {key("Shared.bin")}},
                                                    {key("A.cue"), DiscGroupKind::Cue, {key("Shared.bin")}}},
                                                   folder);
        CHECK(grouping.groups.size() == 2);
        CHECK(grouping.memberHeads.size() == 1);
        CHECK(grouping.headOf(key("Shared.bin"))->relativePath() == "A.cue");
    }
    SECTION("a cycle keeps its first head") {
        const auto grouping = rv::esde::groupDiscs(psx(),
                                                   {{key("B.m3u"), DiscGroupKind::M3u, {key("A.m3u"), key("B.bin")}},
                                                    {key("A.m3u"), DiscGroupKind::M3u, {key("B.m3u")}},
                                                    {key("Self.m3u"), DiscGroupKind::M3u, {key("Self.m3u")}}},
                                                   folder);
        REQUIRE(grouping.groups.size() == 2);
        CHECK(paths(grouping.groups[0].members) == std::vector<std::string>{"B.m3u"});
        CHECK(paths(grouping.groups[1].members) == std::vector<std::string>{"B.bin"});
        CHECK(grouping.headOf(key("A.m3u")) == nullptr);
        CHECK(grouping.headOf(key("Self.m3u")) == nullptr);
        REQUIRE(grouping.issues.size() == 2);
        CHECK(grouping.issues[0].code == "disc-group-cycle");
        CHECK(grouping.issues[0].file == "C:\\Roms\\psx\\B.m3u");
        CHECK(grouping.issues[1].file == "C:\\Roms\\psx\\Self.m3u");
    }
    SECTION("a three-step cycle") {
        const auto grouping = rv::esde::groupDiscs(psx(),
                                                   {{key("C.m3u"), DiscGroupKind::M3u, {key("A.m3u")}},
                                                    {key("B.m3u"), DiscGroupKind::M3u, {key("C.m3u")}},
                                                    {key("A.m3u"), DiscGroupKind::M3u, {key("B.m3u")}}},
                                                   folder);
        CHECK(grouping.groups.size() == 2);
        CHECK(grouping.headOf(key("A.m3u")) == nullptr);
        CHECK(grouping.headOf(key("B.m3u"))->relativePath() == "A.m3u");
        CHECK(grouping.headOf(key("C.m3u"))->relativePath() == "B.m3u");
        CHECK(grouping.issues.size() == 1);
    }
    SECTION("a head without members forms no group") {
        const auto grouping = rv::esde::groupDiscs(psx(), {{key("Empty.cue"), DiscGroupKind::Cue, {}}}, folder);
        CHECK(grouping.groups.empty());
        CHECK(grouping.memberHeads.empty());
    }
}
