// The fixture materializer (tests/support/fixture.hpp) on tests/fixtures/materializer and on broken fixtures written
// here: the copy, the root written into text files as their syntax needs it, generated payloads, times, attributes and
// empty folders (also past MAX_PATH), a fresh folder each time, removed afterwards.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/tree_walk.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::test::Fixture;

namespace {

std::string read(const fs::path& file) {
    const auto text = rv::core::readTextFile(file);
    INFO(rv::core::pathToUtf8(file));
    REQUIRE(text.has_value());
    return *text;
}

/// A fixture source written by the test: `files.json` and one copied file.
fs::path brokenFixture(const rv::test::TempDir& dir, std::string_view manifest) {
    const fs::path source = dir.file("source");
    REQUIRE(rv::core::writeTextFileAtomic(source / "roms" / "kept.txt", "kept").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(source / "files.json", manifest).has_value());
    return source;
}

} // namespace

TEST_CASE("support/fixture: the tree, its root in text files and generated files", "[support][fixture]") {
    auto materialized = Fixture::materialize("materializer");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    const Fixture& fixture = *materialized;
    const fs::path& root = fixture.root();
    const std::string rootText = rv::core::pathToUtf8(root);
    CHECK(root.is_absolute());
    CHECK(root.parent_path().filename() == "test-fixtures");
    CHECK(root.filename().string().starts_with("materializer-"));

    // XML: the root escaped for XML (the build path holds no '&' or '<' here, so it reads as it is).
    const std::string settings = read(fixture.path("es-de/settings/es_settings.xml"));
    CHECK(settings.find("value=\"" + rootText + "/media\"") != std::string::npos);
    CHECK(settings.find("value=\"" + rootText + "\\roms\"") != std::string::npos);
    CHECK(settings.find("{{FIXTURE_ROOT}}") == std::string::npos);
    // JSON: escaped for a string, so the document reads back the path itself.
    const auto library = rv::core::readJsonFile(fixture.path("config/library.json"));
    REQUIRE(library.has_value());
    CHECK((*library)["home"] == rootText + "/es-de");
    CHECK((*library)["romRoot"] == rootText + "\\roms");
    // Plain text: as it is, every occurrence.
    CHECK(read(fixture.path("notes.txt")) == "root=" + rootText + "\nagain=" + rootText + "\n");
    // Other files are copied untouched; files.json is not copied.
    CHECK(read(fixture.path("untouched.dat")).find("{{FIXTURE_ROOT}} stays") != std::string::npos);
    CHECK_FALSE(fs::exists(fixture.path("files.json")));

    // Generated payloads: sizes, the same bytes for the same seed, a shorter one a prefix.
    const std::string alpha = read(fixture.path("roms/snes/Alpha Quest (World).sfc"));
    const std::string copy = read(fixture.path("roms/snes/Alpha Quest (World) [copy].sfc"));
    const std::string beta = read(fixture.path("roms/snes/Beta Run (Europe).sfc"));
    CHECK(alpha.size() == 70000);
    CHECK(rv::core::sha256Hex(alpha) == rv::core::sha256Hex(copy));
    CHECK(rv::core::sha256Hex(alpha) != rv::core::sha256Hex(beta));
    CHECK(alpha == rv::test::payloadBytes(7, 70000));
    CHECK(read(fixture.path("roms/snes/prefix.sfc")) == alpha.substr(0, 13));
    CHECK(fs::file_size(fixture.path("roms/gb/empty.gb")) == 0);
    CHECK(fs::file_size(fixture.path("roms/gb/Pok\xC3\xA9mon Puzzle (EU).gb")) == 16);

    // A last write time and the hidden attribute.
    const auto expected = rv::core::parseUtcIso8601("2001-02-03T04:05:06Z").value();
    const auto dated = fs::last_write_time(fixture.path("roms/gb/Dated (USA).gb"));
    CHECK(std::chrono::clock_cast<std::chrono::system_clock>(dated) == expected);
    const DWORD attributes = GetFileAttributesW(fixture.path("roms/gb/.hidden.gb").c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    CHECK((attributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK((GetFileAttributesW(fixture.path("roms/snes/prefix.sfc").c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0);
    // An empty folder.
    CHECK(fs::is_directory(fixture.path("roms/psx/empty")));
    CHECK(fs::is_empty(fixture.path("roms/psx/empty")));
}

TEST_CASE("support/fixture: a fresh folder each time, removed afterwards", "[support][fixture]") {
    fs::path first;
    {
        auto fixture = Fixture::materialize("materializer");
        REQUIRE(fixture.has_value());
        first = fixture->root();
        auto second = Fixture::materialize("materializer");
        REQUIRE(second.has_value());
        CHECK(second->root() != first);
        REQUIRE(fs::exists(first));
    }
    CHECK_FALSE(fs::exists(first));

    fs::path kept;
    {
        auto fixture = Fixture::materialize("materializer");
        REQUIRE(fixture.has_value());
        fixture->keep();
        kept = fixture->root();
    }
    CHECK(fs::exists(kept));
    fs::remove_all(kept);
}

TEST_CASE("support/fixture: generated files and folders past MAX_PATH", "[support][fixture]") {
    rv::test::TempDir dir;
    std::string deep;
    for (int level = 0; level < 4; ++level) {
        deep += std::string(60, 'd') + std::to_string(level) + "/";
    }
    const std::string file = deep + "Kodomo Chousadan Mighty Pockets - Chousa 3 (Japan) (SoundLink) [b].bs";
    const std::string folder = deep + "Empty Folder With A Long Name, Kept By The Manifest";
    const fs::path source = dir.file("source");
    const std::string manifest = R"({"files": [{"path": ")" + file +
                                 R"(", "size": 300, "seed": 5, "mtime": "2001-02-03T04:05:06Z", "hidden": true}],)" +
                                 R"( "directories": [")" + folder + R"("]})";
    REQUIRE(rv::core::writeTextFileAtomic(source / "short.txt", "root={{FIXTURE_ROOT}}").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(source / "files.json", manifest).has_value());

    auto materialized = Fixture::materializeFrom(source, "long");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    const Fixture& fixture = *materialized;
    REQUIRE(fixture.path(file).native().size() > 260);
    // Read back through \\?\ paths, as the importer reads such trees.
    CHECK(rv::core::readFileShared(fixture.path(file), 1024).value() == rv::test::payloadBytes(5, 300));
    const auto entries = rv::core::listTree(fixture.root());
    REQUIRE(entries.has_value());
    bool foundFile = false;
    bool foundFolder = false;
    for (const rv::core::TreeEntry& entry : *entries) {
        CHECK(entry.kind != rv::core::TreeEntryKind::Error);
        if (entry.relativePath == file) {
            foundFile = true;
            CHECK(entry.size == 300);
            CHECK(entry.hidden);
            CHECK(std::chrono::clock_cast<std::chrono::system_clock>(entry.lastWriteTime) ==
                  rv::core::parseUtcIso8601("2001-02-03T04:05:06Z").value());
        }
        foundFolder = foundFolder || (entry.relativePath == folder && entry.kind == rv::core::TreeEntryKind::Directory);
    }
    CHECK(foundFile);
    CHECK(foundFolder);
}

TEST_CASE("support/fixture: a program of the build copied into a fixture", "[support][fixture]") {
    const fs::path program = rv::test::testProgramsDir() / "rv_fake_emulator.exe";
    if (!fs::exists(program)) {
        SKIP("rv_fake_emulator.exe is not next to the tests");
    }
    rv::test::TempDir dir;
    const fs::path source = dir.file("source");
    REQUIRE(rv::core::writeTextFileAtomic(
                source / "files.json",
                R"({"files": [{"path": "Programs/Emulators/fake-emulator.exe", "copyOf": "rv_fake_emulator.exe",
                               "mtime": "2001-02-03T04:05:06Z"}]})")
                .has_value());
    auto materialized = Fixture::materializeFrom(source, "copy");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    const fs::path copy = materialized->path("Programs/Emulators/fake-emulator.exe");
    CHECK(rv::core::sha256Hex(read(copy)) == rv::core::sha256Hex(read(program)));
    CHECK(std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(copy)) ==
          rv::core::parseUtcIso8601("2001-02-03T04:05:06Z").value());
}

TEST_CASE("support/fixture: a variant made by replacing text in the materialized copy", "[support][fixture]") {
    auto materialized = Fixture::materialize("materializer");
    REQUIRE(materialized.has_value());
    const Fixture& fixture = *materialized;
    const std::string settings = "es-de/settings/es_settings.xml";
    REQUIRE(fixture
                .replaceText(settings, "name=\"ShowHiddenFiles\" value=\"true\"",
                             "name=\"ShowHiddenFiles\" value=\"false\"")
                .has_value());
    const std::string variant = read(fixture.path(settings));
    CHECK(variant.find("name=\"ShowHiddenFiles\" value=\"false\"") != std::string::npos);
    CHECK(variant.find("value=\"true\"") == std::string::npos);
    CHECK(variant.find(rv::core::pathToUtf8(fixture.root()) + "/media") != std::string::npos); // the rest unchanged
    // The source tree is never changed.
    CHECK(read(rv::test::fixturesDir() / "materializer" / "es-de" / "settings" / "es_settings.xml")
              .find("name=\"ShowHiddenFiles\" value=\"true\"") != std::string::npos);

    // The text must be there exactly once.
    const auto missing = fixture.replaceText(settings, "name=\"ShowHiddenGames\"", "x");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    const auto twice = fixture.replaceText("notes.txt", "=", "==");
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_FALSE(fixture.replaceText("no such file.txt", "a", "b").has_value());
}

TEST_CASE("support/fixture: payload bytes", "[support][fixture]") {
    CHECK(rv::test::payloadBytes(1, 0).empty());
    CHECK(rv::test::payloadBytes(1, 20).size() == 20);
    CHECK(rv::test::payloadBytes(1, 20) == rv::test::payloadBytes(1, 20));
    CHECK(rv::test::payloadBytes(1, 20) != rv::test::payloadBytes(2, 20));
    CHECK(rv::test::payloadBytes(1, 64).substr(0, 20) == rv::test::payloadBytes(1, 20));
}

TEST_CASE("support/fixture: broken fixtures are refused", "[support][fixture]") {
    rv::test::TempDir dir;
    const auto refused = [&](std::string_view manifest, std::string_view why) {
        INFO(manifest);
        const fs::path source = brokenFixture(dir, manifest);
        const auto fixture = Fixture::materializeFrom(source, "broken");
        REQUIRE_FALSE(fixture.has_value());
        INFO(fixture.error().toString());
        CHECK(fixture.error().message.find(why) != std::string::npos);
    };
    refused(R"({"files": [{"path": "../outside.bin", "size": 1, "seed": 1}]})", "\"..\"");
    refused(R"({"files": [{"path": "C:/outside.bin", "size": 1, "seed": 1}]})", "outside");
    refused(R"({"files": [{"path": "roms/a.bin", "size": 1, "seed": 1, "colour": "red"}]})", "unknown key");
    refused(R"({"payloads": []})", "unknown key");
    refused(R"({"files": [{"path": "roms/a.bin", "size": -1, "seed": 1}]})", "integers");
    refused(R"({"files": [{"path": "roms/a.bin", "seed": 1}]})", "integers");
    refused(R"({"files": [{"path": "roms/a.bin", "size": 67108865, "seed": 1}]})", "64 MiB");
    refused(R"({"files": [{"path": "roms/kept.txt", "size": 1, "seed": 1}]})", "would replace");
    refused(R"({"files": [{"path": "ROMS/KEPT.TXT", "size": 4, "seed": 1}]})", "would replace"); // the same file
    refused(R"({"files": [{"path": "roms/a.bin", "size": 2, "seed": 1},
                          {"path": "roms/a.bin", "size": 2, "seed": 1}]})",
            "listed twice");
    refused(R"({"files": [{"path": "roms/a.bin", "size": 2, "seed": 1},
                          {"path": "roms/A.BIN", "size": 9, "seed": 1}]})",
            "listed twice");
    refused(R"({"files": [{"path": "roms/a.bin", "size": 1, "seed": 1, "mtime": "yesterday"}]})", "UTC time");
    refused(R"({"files": [{"path": "roms/a.bin", "size": 1, "seed": 1, "hidden": "yes"}]})", "true or false");
    refused(R"({"directories": [7]})", "string");
    refused(R"([])", "object");
    refused(R"({"files": [{"path": "bin/a.exe", "copyOf": "rv_fake_emulator.exe", "size": 1, "seed": 1}]})",
            "either \"copyOf\"");
    refused(R"({"files": [{"path": "bin/a.exe", "copyOf": "../rv_fake_emulator.exe"}]})", "not a path");
    refused(R"({"files": [{"path": "bin/a.exe", "copyOf": "C:rv_fake_emulator.exe"}]})", "not a path");
    refused(R"({"files": [{"path": "bin/a.exe", "copyOf": 7}]})", "file name");
    refused(R"({"files": [{"path": "bin/a.exe", "copyOf": "no such program.exe"}]})", "not beside the test program");

    const auto missing = Fixture::materialize("no such fixture");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
}
