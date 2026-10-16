// ES-DE gamelists read into the catalog's values (rv/esde/gamelist.hpp, I2 step 3): the standard fixture's three
// gamelists and the legacy one that nobody reads, then gamelists generated at test time into a temporary folder (BOM,
// CRLF and other encodings cannot be committed: .gitattributes would normalize them): paths and their traversal,
// duplicates and the entry a listed file takes, files that are broken while ES-DE writes them, values as real
// gamelists write them, unknown, repeated and ignored elements, huge fields and paths, floods of issues kept bounded,
// control characters, deep nesting, and the log, which never shows the user's path above debug level.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <spdlog/spdlog.h>

#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/gamelist.hpp>

#include "support/fixture.hpp"
#include "support/log_capture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::catalog::IssueSeverity;
using rv::catalog::LocalDateTime;
using rv::catalog::Players;
using rv::catalog::RawElement;
using rv::catalog::ReleaseDate;
using rv::esde::Gamelist;
using rv::esde::GamelistEntry;
using rv::esde::GamelistEntryKind;

namespace {

rv::catalog::SystemName systemNamed(std::string_view name) {
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

rv::test::Fixture materializeStandard() {
    auto materialized = rv::test::Fixture::materialize("esde/standard");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    return std::move(*materialized);
}

/// A system's gamelist of the standard fixture, read from where ES-DE keeps it.
Gamelist readStandard(const rv::test::Fixture& fixture, std::string_view system) {
    const fs::path file = rv::esde::gamelistPath(fixture.path("ES-DE"), systemNamed(system));
    auto read = rv::esde::readGamelist(file, systemNamed(system), fixture.path("roms/" + std::string(system)));
    INFO(system << ": " << (read ? "read" : read.error().toString()));
    REQUIRE(read.has_value());
    return std::move(*read);
}

/// A gamelist written at test time and the absolute ROM folder its paths are resolved against (it need not exist:
/// resolving reads nothing on disk).
struct Scratch {
    rv::test::TempDir dir{"rv-gamelist"};
    fs::path romFolder = dir.path() / "Roms" / "gb";

    fs::path write(std::string_view content, std::string_view name = "gamelist.xml") const {
        const fs::path file = dir.file(name);
        REQUIRE(rv::core::writeTextFileAtomic(file, content).has_value());
        return file;
    }
    Gamelist readFile(const fs::path& file, std::uintmax_t maxBytes = rv::esde::kMaxGamelistBytes) const {
        auto read = rv::esde::readGamelist(file, systemNamed("gb"), romFolder, maxBytes);
        INFO((read ? "read" : read.error().toString()));
        REQUIRE(read.has_value());
        return std::move(*read);
    }
    /// readFile() of `content` written as gamelist.xml.
    Gamelist read(std::string_view content) const { return readFile(write(content)); }
    rv::Error failureOfFile(const fs::path& file, std::uintmax_t maxBytes = rv::esde::kMaxGamelistBytes) const {
        const auto read = rv::esde::readGamelist(file, systemNamed("gb"), romFolder, maxBytes);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().context == rv::core::pathToUtf8(file));
        return read.error();
    }
    /// failureOfFile() of `content` written as gamelist.xml.
    rv::Error failure(std::string_view content) const { return failureOfFile(write(content)); }
};

/// A gamelist as ES-DE writes it around `body`.
std::string gameList(std::string_view body) {
    return "<?xml version=\"1.0\"?>\n<gameList>\n" + std::string(body) + "</gameList>\n";
}

const GamelistEntry& entryAt(const Gamelist& gamelist, std::string_view path) {
    const GamelistEntry* entry = gamelist.find(path);
    INFO("the entry " << path);
    REQUIRE(entry != nullptr);
    return *entry;
}

std::vector<std::string> pathsOf(const Gamelist& gamelist) {
    std::vector<std::string> paths;
    for (const GamelistEntry& entry : gamelist.entries) {
        paths.push_back(entry.path);
    }
    return paths;
}

std::vector<std::string> codesOf(const Gamelist& gamelist) {
    std::vector<std::string> codes;
    for (const rv::catalog::Issue& issue : gamelist.issues) {
        codes.push_back(issue.code);
    }
    return codes;
}

/// The issues with `code`, in order.
std::vector<rv::catalog::Issue> issuesWith(const Gamelist& gamelist, std::string_view code) {
    std::vector<rv::catalog::Issue> found;
    for (const rv::catalog::Issue& issue : gamelist.issues) {
        if (issue.code == code) {
            found.push_back(issue);
        }
    }
    return found;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The issue with `code` whose reason holds `part`.
const rv::catalog::Issue& issueAbout(const Gamelist& gamelist, std::string_view code, std::string_view part) {
    const rv::catalog::Issue* found = nullptr;
    for (const rv::catalog::Issue& issue : gamelist.issues) {
        if (found == nullptr && issue.code == code && contains(issue.reason, part)) {
            found = &issue;
        }
    }
    INFO("an issue " << code << " about " << part);
    REQUIRE(found != nullptr);
    return *found;
}

std::string relativePathOf(const GamelistEntry& entry) {
    return entry.key.relativePath();
}

std::string keyOf(std::string_view relativePath) {
    auto key = rv::catalog::makeLookupKey(relativePath);
    REQUIRE(key.has_value());
    return key->key();
}

/// `text` in UTF-16LE with its byte order mark.
std::string utf16le(std::u32string_view text) {
    std::string out = "\xFF\xFE";
    const auto put = [&](std::uint32_t unit) {
        out += static_cast<char>(unit & 0xFF);
        out += static_cast<char>((unit >> 8) & 0xFF);
    };
    for (const char32_t c : text) {
        if (c >= 0x10000) {
            put(0xD800 + ((c - 0x10000) >> 10));
            put(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            put(c);
        }
    }
    return out;
}

/// Holds a file open without sharing: whoever opens it meanwhile fails.
class ExclusiveLock {
public:
    explicit ExclusiveLock(const fs::path& file)
        : m_handle(CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
    }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    ~ExclusiveLock() { release(); }

    [[nodiscard]] bool held() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }
    void release() noexcept {
        if (held()) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE m_handle;
};

} // namespace

TEST_CASE("esde/gamelist: the standard fixture's gb gamelist", "[esde][gamelist][fixture]") {
    const rv::test::Fixture fixture = materializeStandard();
    const Gamelist gb = readStandard(fixture, "gb");

    CHECK(gb.alternativeEmulator.empty());
    CHECK(gb.count(GamelistEntryKind::Game) == 5);
    CHECK(gb.count(GamelistEntryKind::Folder) == 1);
    CHECK(pathsOf(gb) == std::vector<std::string>{"./Tiny Tennis (Europe).gb", "./Collection A",
                                                  "./Collection A/Pocket Puzzle (World).gb",
                                                  "./Caf\xC3\xA9 Racer (Europe).gb", "./Gone Fishing (USA).gb",
                                                  "./../snes/Alpha Quest (USA).sfc"});

    // Every field, decoded, with the original forms of the normalized ones in document order.
    const GamelistEntry& tiny = entryAt(gb, "./Tiny Tennis (Europe).gb");
    CHECK(tiny.kind == GamelistEntryKind::Game);
    CHECK(tiny.line == 3);
    CHECK(relativePathOf(tiny) == "Tiny Tennis (Europe).gb");
    CHECK(tiny.key.key() == "TINY TENNIS (EUROPE).GB");
    CHECK(tiny.metadata.name == "Tiny Tennis");
    CHECK(tiny.metadata.description == "Serve and volley on twelve invented courts.\n\nSingles & doubles, a practice "
                                       "wall and a tournament of eight rounds.");
    CHECK(tiny.metadata.rating == rv::catalog::Rating{7});
    CHECK(tiny.metadata.releaseDate == ReleaseDate{1994, 3, 12});
    CHECK(tiny.metadata.developer == "Pixel Orchard");
    CHECK(tiny.metadata.publisher == "Northwind Games");
    CHECK(tiny.metadata.genre == "Sport / Tennis");
    CHECK(tiny.metadata.players == Players{1, 2});
    CHECK(tiny.flags.favorite);
    CHECK_FALSE(tiny.flags.hidden);
    CHECK(tiny.flags.playCount == 3);
    CHECK(tiny.flags.playTimeSeconds == 1825);
    CHECK(tiny.flags.lastPlayed == LocalDateTime{2025, 8, 12, 21, 15, 3});
    CHECK(tiny.raw.originalForms == std::vector<RawElement>{{"rating", "0.7"},
                                                            {"releasedate", "19940312T000000"},
                                                            {"players", "1-2"},
                                                            {"favorite", "true"},
                                                            {"playcount", "3"},
                                                            {"playtime", "1825"},
                                                            {"lastplayed", "20250812T211503"}});
    CHECK(tiny.raw.unknownElements.empty());
    CHECK(tiny.altEmulator.empty());

    // A folder entry: path, name and the lastplayed ES-DE writes when a game inside it is played.
    const GamelistEntry& collection = entryAt(gb, "./Collection A");
    CHECK(collection.kind == GamelistEntryKind::Folder);
    CHECK(collection.line == 20);
    CHECK(relativePathOf(collection) == "Collection A");
    CHECK(collection.metadata.name == "Collection A");
    CHECK(collection.flags.lastPlayed == LocalDateTime{2025, 9, 1, 19, 30, 0});

    // A year-only date and an unknown element, kept as written.
    const GamelistEntry& puzzle = entryAt(gb, "./Collection A/Pocket Puzzle (World).gb");
    CHECK(puzzle.line == 25);
    CHECK(relativePathOf(puzzle) == "Collection A/Pocket Puzzle (World).gb");
    CHECK(puzzle.metadata.releaseDate == ReleaseDate{1990, 1, 1});
    CHECK(puzzle.metadata.players == Players{1, 1});
    CHECK(puzzle.raw.unknownElements == std::vector<RawElement>{{"region", "wor"}});

    // A non-ASCII name, a trailing space kept as written, a comma inside a genre, the game's alternative emulator.
    const GamelistEntry& cafe = entryAt(gb, "./Caf\xC3\xA9 Racer (Europe).gb");
    CHECK(cafe.line == 36);
    CHECK(relativePathOf(cafe) == "Caf\xC3\xA9 Racer (Europe).gb");
    CHECK(cafe.key.key() == "CAF\xC3\x89 RACER (EUROPE).GB");
    CHECK(cafe.metadata.name == "Caf\xC3\xA9 Racer");
    CHECK(cafe.metadata.publisher == "Northwind Games ");
    CHECK(rv::catalog::displayText(cafe.metadata.publisher) == "Northwind Games");
    CHECK(cafe.metadata.genre == "Corsa, Guida / Moto");
    CHECK(cafe.metadata.players == Players{2, 16});
    CHECK(cafe.altEmulator == "Fake Emulator (Game Folder)");

    // The parser checks nothing on disk: an entry whose file is gone is an entry like any other.
    const GamelistEntry& gone = entryAt(gb, "./Gone Fishing (USA).gb");
    CHECK(gone.line == 47);
    CHECK_FALSE(gone.outsideSystemFolder());
    CHECK_FALSE(fs::exists(fixture.path("roms/gb/Gone Fishing (USA).gb")));

    // A path that leads out of gb's folder: kept without a key, reported.
    const GamelistEntry& wrongWay = entryAt(gb, "./../snes/Alpha Quest (USA).sfc");
    CHECK(wrongWay.outsideSystemFolder());
    CHECK(wrongWay.key.empty());
    CHECK(wrongWay.line == 55);
    CHECK(wrongWay.metadata.name == "Wrong Way");
    REQUIRE(gb.issues.size() == 1);
    const rv::catalog::Issue& outside = gb.issues[0];
    CHECK(outside.code == rv::esde::kGamelistIssuePathOutsideFolder);
    CHECK(outside.severity == IssueSeverity::Warning);
    CHECK(outside.system == "gb");
    CHECK(outside.file == rv::core::pathToUtf8(rv::esde::gamelistPath(fixture.path("ES-DE"), systemNamed("gb"))));
    CHECK(contains(outside.reason, "line 55"));
    CHECK(contains(outside.reason, "\"./../snes/Alpha Quest (USA).sfc\""));

    // Reading is deterministic.
    CHECK(readStandard(fixture, "gb") == gb);
}

TEST_CASE("esde/gamelist: the standard fixture's snes and psx gamelists", "[esde][gamelist][fixture]") {
    const rv::test::Fixture fixture = materializeStandard();

    const Gamelist snes = readStandard(fixture, "snes");
    CHECK(snes.alternativeEmulator == "Fake Emulator (Hidden Console)"); // beside <gameList>, before it
    CHECK(snes.count(GamelistEntryKind::Game) == 4);
    CHECK(snes.count(GamelistEntryKind::Folder) == 2);
    CHECK(snes.issues.empty());
    CHECK(pathsOf(snes) == std::vector<std::string>{"./Alpha Quest (USA).sfc", "./Mixed Case (USA).Sfc", "./Hacks",
                                                    "./Hacks/Gamma Hack (Hack).sfc", "./delta drift (usa).sfc",
                                                    "./Sports Vol. 2"});

    const GamelistEntry& alpha = entryAt(snes, "./Alpha Quest (USA).sfc");
    CHECK(alpha.line == 6);
    CHECK(alpha.metadata.rating == rv::catalog::Rating{10});
    CHECK(alpha.metadata.players == Players{8, std::nullopt}); // "8+"
    CHECK(alpha.flags.kidGame);
    CHECK(alpha.metadata.developer == "Lantern Works ");
    CHECK(alpha.metadata.publisher == "Mirage Soft\n"); // a line feed ending the value
    CHECK(rv::catalog::displayText(alpha.metadata.publisher) == "Mirage Soft");
    // "&amp;amp;" decodes once: the description shows "&amp;", as in ES-DE.
    CHECK(contains(alpha.metadata.description, "\"Ash &amp; Ember\""));
    CHECK(alpha.raw.originalForms ==
          std::vector<RawElement>{{"rating", "1"}, {"releasedate", "19951120T000000"}, {"players", "8+"},
                                  {"kidgame", "true"}});

    // The parser knows no extensions: the .Sfc entry is an entry; the scan makes it "not a game".
    CHECK(entryAt(snes, "./Mixed Case (USA).Sfc").kind == GamelistEntryKind::Game);
    const GamelistEntry& hacks = entryAt(snes, "./Hacks");
    CHECK(hacks.kind == GamelistEntryKind::Folder);
    CHECK(hacks.metadata.description == "Fan-made changes of other games.");
    const GamelistEntry& gamma = entryAt(snes, "./Hacks/Gamma Hack (Hack).sfc");
    CHECK(relativePathOf(gamma) == "Hacks/Gamma Hack (Hack).sfc");
    CHECK(gamma.metadata.rating == rv::catalog::Rating{3});
    CHECK(gamma.metadata.players == Players{2, 3000});

    // The case-only difference: the relative path stays as written, the key is the file's on disk.
    const GamelistEntry& delta = entryAt(snes, "./delta drift (usa).sfc");
    CHECK(delta.line == 35);
    CHECK(relativePathOf(delta) == "delta drift (usa).sfc");
    CHECK(delta.key.key() == keyOf("Delta Drift (USA).sfc"));
    CHECK(fs::exists(fixture.path("roms/snes/Delta Drift (USA).sfc")));

    const GamelistEntry& sports = entryAt(snes, "./Sports Vol. 2");
    CHECK(sports.kind == GamelistEntryKind::Folder);
    CHECK(sports.line == 44);
    CHECK(relativePathOf(sports) == "Sports Vol. 2");
    CHECK(sports.metadata.name == "Sports, Volume 2");

    const Gamelist psx = readStandard(fixture, "psx");
    CHECK(psx.alternativeEmulator.empty());
    CHECK(psx.count(GamelistEntryKind::Game) == 2);
    CHECK(psx.count(GamelistEntryKind::Folder) == 0);
    CHECK(psx.issues.empty());
    const GamelistEntry& twin = entryAt(psx, "./Twin Tracks (Europe).cue");
    CHECK(twin.line == 3);
    CHECK(twin.metadata.rating == rv::catalog::Rating{5});
    CHECK(twin.metadata.players == Players{1, 4});
    CHECK(twin.flags.playCount == 12);
    CHECK(twin.flags.playTimeSeconds == 33552);
    CHECK(twin.flags.lastPlayed == LocalDateTime{2026, 9, 6, 19, 4, 54});
    const GamelistEntry& solo = entryAt(psx, "./Solo Disc (USA).chd");
    CHECK(solo.line == 17);
    CHECK(solo.metadata.players == Players{24, 24});
    CHECK(solo.flags.hidden);
    CHECK(solo.flags.completed);
    CHECK_FALSE(solo.flags.broken);
    CHECK(solo.raw.originalForm("broken") != nullptr);
    CHECK(*solo.raw.originalForm("broken") == "false");

    // The README's gamelist entries: 11 <game> and 3 <folder>.
    const Gamelist gb = readStandard(fixture, "gb");
    CHECK(gb.count(GamelistEntryKind::Game) + snes.count(GamelistEntryKind::Game) +
              psx.count(GamelistEntryKind::Game) ==
          11);
    CHECK(gb.count(GamelistEntryKind::Folder) + snes.count(GamelistEntryKind::Folder) +
              psx.count(GamelistEntryKind::Folder) ==
          3);
}

TEST_CASE("esde/gamelist: the legacy gamelist inside a ROM folder is never read", "[esde][gamelist][fixture]") {
    const rv::test::Fixture fixture = materializeStandard();
    const auto gb = systemNamed("gb");
    const fs::path legacy = fixture.path("roms/gb/gamelist.xml");

    // Gamelists are read from the ES-DE home only.
    const fs::path path = rv::esde::gamelistPath(fixture.path("ES-DE"), gb);
    CHECK(path == fixture.path("ES-DE") / "gamelists" / "gb" / "gamelist.xml");
    CHECK_FALSE(rv::core::pathIsWithin(path, fixture.path("roms")));

    // Locked without sharing: reading gb's gamelist never opens the legacy one, and gives the home's metadata.
    ExclusiveLock lock(legacy);
    REQUIRE(lock.held());
    const Gamelist home = readStandard(fixture, "gb");
    CHECK(entryAt(home, "./Tiny Tennis (Europe).gb").metadata.name == "Tiny Tennis");
    const auto locked = rv::esde::readGamelist(legacy, gb, fixture.path("roms/gb"));
    REQUIRE_FALSE(locked.has_value());
    CHECK(locked.error().code == rv::ErrorCode::Conflict); // the lock is real: whoever opened it would fail
    lock.release();

    // What a reader that took it would show: an older scraper's entry, its attributes and image ignored.
    const auto read = rv::esde::readGamelist(legacy, gb, fixture.path("roms/gb"));
    REQUIRE(read.has_value());
    const GamelistEntry& tiny = entryAt(*read, "./Tiny Tennis (Europe).gb");
    CHECK(tiny.metadata.name == "Legacy Tennis");
    CHECK(tiny.metadata.rating == rv::catalog::Rating{8}); // "0.75", two decimals
    CHECK_FALSE(tiny.metadata.releaseDate.has_value());    // <releasedate></releasedate>
    CHECK(tiny.raw.originalForms == std::vector<RawElement>{{"rating", "0.75"}, {"releasedate", ""}});
    CHECK(tiny.raw.unknownElements ==
          std::vector<RawElement>{{"image", "./images/Tiny Tennis (Europe)-image.png"},
                                  {"md5", "00112233445566778899aabbccddeeff"}});
    CHECK(read->issues.empty());
}

TEST_CASE("esde/gamelist: paths resolved against the ROM folder, never leaving it", "[esde][gamelist]") {
    const Scratch scratch;
    const std::string folder = rv::core::pathToUtf8(scratch.romFolder); // absolute, with backslashes
    std::string forward = folder;
    std::replace(forward.begin(), forward.end(), '\\', '/');
    const std::string upper = rv::core::fileSystemUpperCase(folder).value();
    const std::string sibling = rv::core::pathToUtf8(scratch.dir.path() / "Roms" / "gba");
    // One entry per line, the first at line 3.
    const Gamelist read = scratch.read(gameList("<game><path>./Sub/Deeper/a.gb</path></game>\n"
                                                "<game><path>b.gb</path></game>\n"
                                                "<game><path>.\\Back\\c.gb</path></game>\n"
                                                "<game><path>" +
                                                folder + "\\Abs\\d.gb</path></game>\n<game><path>" + forward +
                                                "/Abs/e.gb</path></game>\n<game><path>" + upper +
                                                "\\Abs\\F.gb</path></game>\n"
                                                "<folder><path>./Sub</path></folder>\n"
                                                "<folder><path>./Other/</path></folder>\n"
                                                "<game><path>./a//b/./c.gb</path></game>\n"
                                                "<game><path>./../gba/x.gba</path></game>\n"
                                                "<game><path>" +
                                                sibling +
                                                "\\y.gba</path></game>\n"
                                                "<game><path>Q:\\Roms\\gb\\z.gb</path></game>\n"
                                                "<game><path>\\\\nas\\roms\\gb\\w.gb</path></game>\n"
                                                "<game><path>./a/../b.gb</path></game>\n"
                                                "<game><path>./a.gb:stream</path></game>\n"
                                                "<game><path>C:a.gb</path></game>\n"
                                                "<game><path>/rooted.gb</path></game>\n"
                                                "<game><path>\\\\?\\C:\\x.gb</path></game>\n"
                                                "<game><path>./</path></game>\n"
                                                "<game><path></path><name>Empty path</name></game>\n"
                                                "<game><name>No path</name></game>\n"
                                                "<folder><path>   </path></folder>\n"));

    // Inside the folder: relative forms with or without "./", either separator, absolute paths below it (its own part
    // compared as the file system compares names), folders written with their separator; what lies below is as written.
    const auto relative = [&](const std::string& path) { return relativePathOf(entryAt(read, path)); };
    CHECK(relative("./Sub/Deeper/a.gb") == "Sub/Deeper/a.gb");
    CHECK(relative("b.gb") == "b.gb");
    CHECK(relative(".\\Back\\c.gb") == "Back/c.gb");
    CHECK(relative(folder + "\\Abs\\d.gb") == "Abs/d.gb");
    CHECK(relative(forward + "/Abs/e.gb") == "Abs/e.gb");
    CHECK(relative(upper + "\\Abs\\F.gb") == "Abs/F.gb");
    CHECK(relative("./Sub") == "Sub");
    CHECK(entryAt(read, "./Sub").kind == GamelistEntryKind::Folder);
    CHECK(relative("./Other/") == "Other");
    CHECK(relative("./a//b/./c.gb") == "a/b/c.gb");

    // Outside it: kept without a key, each reported at its line.
    for (const std::string& path : {std::string("./../gba/x.gba"), sibling + "\\y.gba", std::string("Q:\\Roms\\gb\\z.gb"),
                                    std::string("\\\\nas\\roms\\gb\\w.gb")}) {
        INFO(path);
        CHECK(entryAt(read, path).outsideSystemFolder());
    }
    CHECK(read.entries.size() == 13);
    CHECK(read.count(GamelistEntryKind::Folder) == 2);

    const std::string_view outside = rv::esde::kGamelistIssuePathOutsideFolder;
    const std::string_view malformed = rv::esde::kGamelistIssuePathMalformed;
    const std::string_view withoutPath = rv::esde::kGamelistIssueEntryWithoutPath;
    CHECK(codesOf(read) == std::vector<std::string>{std::string(outside), std::string(outside), std::string(outside),
                                                    std::string(outside), std::string(malformed),
                                                    std::string(malformed), std::string(malformed),
                                                    std::string(malformed), std::string(malformed),
                                                    std::string(malformed), std::string(withoutPath),
                                                    std::string(withoutPath), std::string(withoutPath)});
    for (const rv::catalog::Issue& issue : read.issues) {
        CHECK(issue.severity == IssueSeverity::Warning);
        CHECK(issue.system == "gb");
    }
    CHECK(contains(issueAbout(read, outside, "line 12").reason, "\"..\" elements lead outside"));
    CHECK(contains(issueAbout(read, outside, "line 13").reason, "lies outside the folder"));
    CHECK(contains(issueAbout(read, outside, "line 14").reason, "\"Q:\\Roms\\gb\\z.gb\""));
    CHECK(contains(issueAbout(read, malformed, "line 16").reason, "\"..\" elements are not allowed"));
    CHECK(contains(issueAbout(read, malformed, "line 17").reason, "alternate data streams"));
    CHECK(contains(issueAbout(read, malformed, "line 18").reason, "drive-relative"));
    CHECK(contains(issueAbout(read, malformed, "line 19").reason, "rooted without a drive"));
    CHECK(contains(issueAbout(read, malformed, "line 20").reason, "forms are not allowed"));
    CHECK(contains(issueAbout(read, malformed, "line 21").reason, "names the folder itself"));
    CHECK(contains(issueAbout(read, withoutPath, "line 22").reason, "<game> entry"));
    CHECK(contains(issueAbout(read, withoutPath, "line 23").reason, "<game> entry"));
    CHECK(contains(issueAbout(read, withoutPath, "line 24").reason, "<folder> entry")); // blanks are no text
}

TEST_CASE("esde/gamelist: entries naming the same file, the last of a kind and path counts", "[esde][gamelist]") {
    const Scratch scratch;
    const Gamelist read = scratch.read(gameList("<game><path>./Same.gb</path><name>First</name></game>\n"         // 3
                                                "<game><path>./Other.gb</path><name>Other</name></game>\n"        // 4
                                                "<game><path>./same.gb</path><name>Case only</name></game>\n"     // 5
                                                "<game><path>./Same.gb</path><name>Last</name></game>\n"          // 6
                                                "<folder><path>./Dir</path><name>Folder</name></folder>\n"       // 7
                                                "<game><path>./Dir</path><name>Named like it</name></game>\n"    // 8
                                                "<game><path>./../out.gb</path><name>Out 1</name></game>\n"      // 9
                                                "<game><path>./../out.gb</path><name>Out 2</name></game>\n"      // 10
                                                "<game><path>./../OUT.gb</path><name>Out 3</name></game>\n"      // 11
                                                "<game><path>./Exact.gb</path><name>E1</name></game>\n"          // 12
                                                "<game><path>.\\Exact.gb</path><name>E2</name></game>\n"));      // 13

    // In document order, the last entry of each kind and relative path (outside the folder: of each path as written).
    // Entries of one file that differ in kind or in case all stay: ES-DE applies the one that matches the disk.
    CHECK(pathsOf(read) == std::vector<std::string>{"./Other.gb", "./same.gb", "./Same.gb", "./Dir", "./Dir",
                                                    "./../out.gb", "./../OUT.gb", ".\\Exact.gb"});
    std::vector<std::string> names;
    for (const GamelistEntry& entry : read.entries) {
        names.push_back(entry.metadata.name);
    }
    CHECK(names ==
          std::vector<std::string>{"Other", "Case only", "Last", "Folder", "Named like it", "Out 2", "Out 3", "E2"});
    CHECK(entryAt(read, "./Same.gb").line == 6);
    REQUIRE(read.find("./Dir", GamelistEntryKind::Folder) != nullptr);
    CHECK(read.find("./Dir", GamelistEntryKind::Folder)->metadata.name == "Folder");
    REQUIRE(read.find("./Dir", GamelistEntryKind::Game) != nullptr);
    CHECK(read.find("./Dir", GamelistEntryKind::Game)->metadata.name == "Named like it");
    CHECK(entryAt(read, "./same.gb").key.key() == entryAt(read, "./Same.gb").key.key());

    const auto duplicates = issuesWith(read, rv::esde::kGamelistIssueDuplicatePath);
    REQUIRE(duplicates.size() == 5);
    for (const rv::catalog::Issue& issue : duplicates) {
        CHECK(issue.severity == IssueSeverity::Warning);
    }
    // Case only: one file as the file system compares names, two for ES-DE, which finds a file by its exact name.
    CHECK(duplicates[0].reason == "the <game> entry at line 5 (\"./same.gb\") names the same file (names compared as "
                                  "the file system compares them) as the <game> entry at line 3 (\"./Same.gb\"): both "
                                  "kept, for the ROM scan to choose by what is on disk");
    CHECK(duplicates[1].reason == "the <game> entry at line 3 (\"./Same.gb\") is superseded by the <game> entry at "
                                  "line 6 (\"./Same.gb\"), which names the same path: the last one counts, as in "
                                  "ES-DE");
    // Across kinds: ES-DE applies the one whose kind matches the file on disk.
    CHECK(duplicates[2].reason == "the <game> entry at line 8 (\"./Dir\") names the same path as the <folder> entry "
                                  "at line 7 (\"./Dir\"): both kept, for the ROM scan to choose by what is on disk");
    // Outside the folder, by path as written (ES-DE skips them all).
    CHECK(duplicates[3].reason == "the <game> entry at line 9 (\"./../out.gb\") is superseded by the <game> entry at "
                                  "line 10 (\"./../out.gb\"), which names the same path: the last one is kept");
    // Written differently, the same relative path.
    CHECK(duplicates[4].reason == "the <game> entry at line 12 (\"./Exact.gb\") is superseded by the <game> entry at "
                                  "line 13 (\".\\Exact.gb\"), which names the same file: the last one counts, as in "
                                  "ES-DE");
    CHECK(issuesWith(read, rv::esde::kGamelistIssuePathOutsideFolder).size() == 3);

    // Superseding entries take the places of earlier ones as they are read; the entries end in document order.
    const Gamelist interleaved = scratch.read(gameList("<game><path>./a.gb</path><name>A1</name></game>\n"
                                                       "<game><path>./b.gb</path><name>B1</name></game>\n"
                                                       "<game><path>./c.gb</path><name>C1</name></game>\n"
                                                       "<game><path>./b.gb</path><name>B2</name></game>\n"
                                                       "<game><path>./a.gb</path><name>A2</name></game>\n"
                                                       "<game><path>./d.gb</path><name>D1</name></game>\n"
                                                       "<game><path>./c.gb</path><name>C2</name></game>\n"));
    std::vector<std::string> kept;
    for (const GamelistEntry& entry : interleaved.entries) {
        kept.push_back(entry.metadata.name + "@" + std::to_string(entry.line));
    }
    CHECK(kept == std::vector<std::string>{"B2@6", "A2@7", "D1@8", "C2@9"});
}

TEST_CASE("esde/gamelist: the entry that applies to a listed file, as ES-DE applies them", "[esde][gamelist]") {
    const Scratch scratch;
    const Gamelist read = scratch.read(gameList("<game><path>./Same.gb</path><name>Exact</name></game>\n"
                                                "<game><path>./SAME.GB</path><name>Upper</name></game>\n"
                                                "<folder><path>./Pack.zip</path><name>Pack folder</name></folder>\n"
                                                "<game><path>./Pack.zip</path><name>Pack game</name></game>\n"
                                                "<folder><path>./Lone.gb</path><name>Lone folder</name></folder>\n"));
    REQUIRE(read.entries.size() == 5);
    // What the ROM scan does with a file it lists: the entries of its key, in the order of Gamelist::entries.
    const auto entryFor = [&](std::string_view listed, GamelistEntryKind kind) -> const GamelistEntry* {
        auto key = rv::catalog::makeLookupKey(listed);
        REQUIRE(key.has_value());
        std::vector<const GamelistEntry*> sameKey;
        for (const GamelistEntry& entry : read.entries) {
            if (entry.key.key() == key->key()) {
                sameKey.push_back(&entry);
            }
        }
        return rv::esde::entryForListed(sameKey, *key, kind);
    };
    const auto nameFor = [&](std::string_view listed, GamelistEntryKind kind) {
        const GamelistEntry* entry = entryFor(listed, kind);
        return entry == nullptr ? std::string("(none)") : entry->metadata.name;
    };

    // A file takes the entry of its kind spelled exactly like it, the one ES-DE applies...
    CHECK(nameFor("Same.gb", GamelistEntryKind::Game) == "Exact");
    CHECK(nameFor("SAME.GB", GamelistEntryKind::Game) == "Upper");
    // ...else the last one of its kind: a case-only difference matches it (ES-DE would show a second entry).
    CHECK(nameFor("same.gb", GamelistEntryKind::Game) == "Upper");
    // A directory ES-DE enters takes the <folder> entry, one named with a configured extension (a game) the <game> one.
    CHECK(nameFor("Pack.zip", GamelistEntryKind::Folder) == "Pack folder");
    CHECK(nameFor("Pack.zip", GamelistEntryKind::Game) == "Pack game");
    CHECK(nameFor("pack.ZIP", GamelistEntryKind::Folder) == "Pack folder");
    // No entry of its kind: the last one of the other kind, which the scan skips as ES-DE does (Game/folder mismatch).
    const GamelistEntry* lone = entryFor("Lone.gb", GamelistEntryKind::Game);
    REQUIRE(lone != nullptr);
    CHECK(lone->kind == GamelistEntryKind::Folder);
    // No entry of its key.
    CHECK(entryFor("Other.gb", GamelistEntryKind::Game) == nullptr);
}

TEST_CASE("esde/gamelist: files that are broken, as while ES-DE writes them, are failures to retry",
          "[esde][gamelist]") {
    const Scratch scratch;
    const std::string valid = gameList("<game><path>./a.gb</path><name>A</name></game>\n"
                                       "<folder><path>./F</path></folder>\n");
    REQUIRE(scratch.read(valid).entries.size() == 2);

    SECTION("a missing file: the system has no gamelist") {
        CHECK(scratch.failureOfFile(scratch.dir.file("missing.xml")).code == rv::ErrorCode::NotFound);
    }
    SECTION("empty, blank, a byte order mark alone") {
        for (const std::string_view content :
             {std::string_view(), std::string_view(" \r\n\t\n"), std::string_view("\xEF\xBB\xBF")}) {
            const rv::Error error = scratch.failure(content);
            CHECK(error.code == rv::ErrorCode::ParseError);
            CHECK(contains(error.message, "empty"));
        }
    }
    SECTION("no <gameList>") {
        const rv::Error alone = scratch.failure(
            "<?xml version=\"1.0\"?>\n<alternativeEmulator>\n\t<label>A</label>\n</alternativeEmulator>\n");
        CHECK(alone.code == rv::ErrorCode::ParseError);
        CHECK(contains(alone.message, "no <gameList> element"));
        CHECK(scratch.failure("<systemList><system/></systemList>").code == rv::ErrorCode::ParseError);
        CHECK(scratch.failure("<?xml version=\"1.0\"?>\n<!-- nothing yet -->\n").code == rv::ErrorCode::ParseError);
        CHECK(scratch.failure("<GameList/>").code == rv::ErrorCode::ParseError); // names compare with their case
    }
    SECTION("cut short anywhere, as ES-DE truncates the file before it writes it again") {
        // Every prefix but the whole document (its last line end aside) fails: no entry is read from half a file.
        for (std::size_t length = 1; length + 1 < valid.size(); ++length) {
            INFO(length << " bytes: " << valid.substr(0, length));
            CHECK(scratch.failure(std::string_view(valid).substr(0, length)).code == rv::ErrorCode::ParseError);
        }
        CHECK(scratch.read(std::string_view(valid).substr(0, valid.size() - 1)).entries.size() == 2);
    }
    SECTION("padded or overwritten with NUL characters") {
        const rv::Error padded = scratch.failure(valid + std::string(64, '\0'));
        CHECK(padded.code == rv::ErrorCode::ParseError);
        CHECK(contains(padded.message, "NUL"));
        std::string holed = valid;
        holed.replace(30, 8, std::string(8, '\0'));
        CHECK(scratch.failure(holed).code == rv::ErrorCode::ParseError);
    }
    SECTION("larger than the cap, a folder, a ROM folder that is not absolute") {
        const fs::path file = scratch.write(valid);
        CHECK(scratch.failureOfFile(file, valid.size() - 1).code == rv::ErrorCode::OutOfRange);
        CHECK(scratch.readFile(file, valid.size()).entries.size() == 2);
        const fs::path folder = scratch.dir.file("folder.xml");
        REQUIRE(fs::create_directory(folder));
        CHECK(scratch.failureOfFile(folder).code == rv::ErrorCode::InvalidArgument);
        const auto relative = rv::esde::readGamelist(file, systemNamed("gb"), fs::path("Roms/gb"));
        REQUIRE_FALSE(relative.has_value());
        CHECK(relative.error().code == rv::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("esde/gamelist: a <gameList> without entries is valid, the alternative emulator on either side",
          "[esde][gamelist]") {
    const Scratch scratch;
    for (const std::string_view content : {std::string_view("<gameList/>"),
                                           std::string_view("<?xml version=\"1.0\"?>\n<gameList>\n</gameList>\n")}) {
        const Gamelist read = scratch.read(content);
        CHECK(read.entries.empty());
        CHECK(read.issues.empty());
        CHECK(read.alternativeEmulator.empty());
    }
    const Gamelist after = scratch.read("<?xml version=\"1.0\"?>\n<gameList>\n</gameList>\n"
                                        "<alternativeEmulator>\n\t<label>Core B</label>\n</alternativeEmulator>\n");
    CHECK(after.alternativeEmulator == "Core B");
    CHECK(after.issues.empty());
}

TEST_CASE("esde/gamelist: the top-level alternative emulator as ES-DE reads it", "[esde][gamelist]") {
    const Scratch scratch;
    const std::string_view ignored = rv::esde::kGamelistIssueIgnoredElement;

    // As written, spaces included: launch translation compares it with the command labels.
    CHECK(scratch.read("<alternativeEmulator><label> Spaced Label </label></alternativeEmulator><gameList/>")
              .alternativeEmulator == " Spaced Label ");

    // Without a label text: none, reported.
    for (const std::string_view content :
         {std::string_view("<alternativeEmulator><label></label></alternativeEmulator>\n<gameList/>"),
          std::string_view("<alternativeEmulator>\n\t<label>  </label>\n</alternativeEmulator>\n<gameList/>"),
          std::string_view("<alternativeEmulator/>\n<gameList/>")}) {
        const Gamelist read = scratch.read(content);
        CHECK(read.alternativeEmulator.empty());
        REQUIRE(read.issues.size() == 1);
        CHECK(read.issues[0].code == ignored);
        CHECK(read.issues[0].severity == IssueSeverity::Info);
        CHECK(contains(read.issues[0].reason, "1 <alternativeEmulator> element(s) without a <label> text, which "
                                              "ES-DE ignores; the first at line 1"));
    }

    // The first label of the first <alternativeEmulator>; the rest is ignored, reported.
    const Gamelist several = scratch.read("<alternativeEmulator>\n<label>A</label>\n<label>B</label>\n<note>x</note>\n"
                                          "</alternativeEmulator>\n<gameList/>\n"
                                          "<alternativeEmulator><label>C</label></alternativeEmulator>\n");
    CHECK(several.alternativeEmulator == "A");
    REQUIRE(several.issues.size() == 3);
    CHECK(several.issues[0].reason == "1 <label> element(s) inside <alternativeEmulator> besides its first <label>, "
                                      "which ES-DE ignores; the first at line 3");
    CHECK(several.issues[1].reason == "1 <note> element(s) inside <alternativeEmulator> besides its first <label>, "
                                      "which ES-DE ignores; the first at line 4");
    CHECK(several.issues[2].reason == "1 top-level <alternativeEmulator> element(s) besides the first <gameList> and "
                                      "the first <alternativeEmulator>, which ES-DE ignores; the first at line 7");

    // Inside <gameList> it is no alternative emulator.
    const Gamelist inside =
        scratch.read("<gameList>\n<alternativeEmulator><label>X</label></alternativeEmulator>\n</gameList>\n");
    CHECK(inside.alternativeEmulator.empty());
    REQUIRE(inside.issues.size() == 1);
    CHECK(inside.issues[0].reason == "1 <alternativeEmulator> element(s) inside <gameList> besides <game> and "
                                     "<folder>, which ES-DE ignores; the first at line 2");
}

TEST_CASE("esde/gamelist: values as real gamelists write them", "[esde][gamelist]") {
    const Scratch scratch;
    const Gamelist read = scratch.read(gameList("<game>\n"                                        // line 3
                                                "\t<path>./Year Only.gb</path>\n"
                                                "\t<name>10 out of 10 :  Dinosaurs</name>\n"
                                                "\t<releasedate>19900101T000000</releasedate>\n"
                                                "\t<rating>0.7</rating>\n"
                                                "\t<players>2-3000</players>\n"
                                                "\t<developer>Mirage Soft </developer>\n"
                                                "\t<publisher>Deja Vu \n</publisher>\n"           // lines 10-11
                                                "\t<desc>First paragraph.\n\nSecond, after a blank line.</desc>\n"
                                                "</game>\n"                                       // line 15
                                                "<game>\n"
                                                "\t<path>./Open Range.gb</path>\n"
                                                "\t<players>8+</players>\n"
                                                "\t<rating>1</rating>\n"
                                                "\t<lastplayed>20260906T190454</lastplayed>\n"
                                                "\t<playcount>3</playcount>\n"
                                                "\t<playtime>363</playtime>\n"
                                                "\t<favorite>true</favorite>\n"
                                                "\t<kidgame>false</kidgame>\n"
                                                "\t<sortname>Range, Open</sortname>\n"
                                                "</game>\n"
                                                "<game>\n"                                        // line 27
                                                "\t<path>./Legacy.gb</path>\n"
                                                "\t<rating>0.75</rating>\n"
                                                "\t<releasedate></releasedate>\n"
                                                "\t<players>   </players>\n"
                                                "</game>\n"
                                                "<game>\n"                                        // line 33
                                                "\t<path>./Unreadable.gb</path>\n"
                                                "\t<rating>4/5</rating>\n"                        // line 35
                                                "\t<players>1, 2</players>\n"
                                                "\t<releasedate>1990</releasedate>\n"
                                                "\t<favorite>True</favorite>\n"                   // line 38
                                                "\t<playcount>-1</playcount>\n"
                                                "\t<lastplayed>0</lastplayed>\n"
                                                "</game>\n"
                                                "<game><path>./Unreadable 2.gb</path><rating>11</rating></game>\n"));

    const GamelistEntry& year = entryAt(read, "./Year Only.gb");
    CHECK(year.metadata.releaseDate == ReleaseDate{1990, 1, 1}); // a year-only date reads as 1 January
    CHECK(year.metadata.rating == rv::catalog::Rating{7});
    CHECK(year.metadata.players == Players{2, 3000});
    CHECK(year.metadata.name == "10 out of 10 :  Dinosaurs"); // text exactly as written...
    CHECK(year.metadata.developer == "Mirage Soft ");
    CHECK(year.metadata.publisher == "Deja Vu \n");
    CHECK(year.metadata.description == "First paragraph.\n\nSecond, after a blank line.");
    CHECK(rv::catalog::displayText(year.metadata.publisher) == "Deja Vu"); // ...cleaned only where shown on one line
    CHECK(rv::catalog::displayText(year.metadata.name) == "10 out of 10 :  Dinosaurs");

    const GamelistEntry& open = entryAt(read, "./Open Range.gb");
    CHECK(open.metadata.players == Players{8, std::nullopt});
    CHECK(open.metadata.rating == rv::catalog::Rating{10});
    CHECK(open.metadata.sortName == "Range, Open");
    CHECK(open.flags.lastPlayed == LocalDateTime{2026, 9, 6, 19, 4, 54});
    CHECK(open.flags.playCount == 3);
    CHECK(open.flags.playTimeSeconds == 363);
    CHECK(open.flags.favorite);
    CHECK_FALSE(open.flags.kidGame);

    // Two decimals round to the nearest tenth; empty elements (blanks are no text) read as missing ones.
    const GamelistEntry& legacy = entryAt(read, "./Legacy.gb");
    CHECK(legacy.metadata.rating == rv::catalog::Rating{8});
    CHECK_FALSE(legacy.metadata.releaseDate.has_value());
    CHECK_FALSE(legacy.metadata.players.has_value());
    CHECK(legacy.raw.originalForms ==
          std::vector<RawElement>{{"rating", "0.75"}, {"releasedate", ""}, {"players", ""}});

    // What does not parse is kept as written, without a value (a boolean false, a count 0, as ES-DE reads them).
    const GamelistEntry& unreadable = entryAt(read, "./Unreadable.gb");
    CHECK_FALSE(unreadable.metadata.rating.has_value());
    CHECK_FALSE(unreadable.metadata.players.has_value());
    CHECK_FALSE(unreadable.metadata.releaseDate.has_value());
    CHECK_FALSE(unreadable.flags.favorite);
    CHECK(unreadable.flags.playCount == 0);
    CHECK_FALSE(unreadable.flags.lastPlayed.has_value());
    CHECK(unreadable.raw.originalForms == std::vector<RawElement>{{"rating", "4/5"},
                                                                  {"players", "1, 2"},
                                                                  {"releasedate", "1990"},
                                                                  {"favorite", "True"},
                                                                  {"playcount", "-1"},
                                                                  {"lastplayed", "0"}});
    // One issue per element name, in the order of their first value.
    REQUIRE(read.issues.size() == 6);
    for (const rv::catalog::Issue& issue : read.issues) {
        CHECK(issue.code == rv::esde::kGamelistIssueValueUnreadable);
        CHECK(issue.severity == IssueSeverity::Info);
    }
    CHECK(read.issues[0].reason == "2 <rating> value(s) that are not a rating (a decimal number from 0 to 1): kept "
                                   "as written, without a value; the first, \"4/5\", at line 35");
    CHECK(contains(read.issues[1].reason, "1 <players> value(s) that are not a number of players"));
    CHECK(contains(read.issues[2].reason, "1 <releasedate> value(s) that are not an ES-DE date-time"));
    CHECK(read.issues[3].reason == "1 <favorite> value(s) that are not an ES-DE boolean (true or false): kept as "
                                   "written, read as false, as ES-DE reads anything but \"true\"; the first, "
                                   "\"True\", at line 38");
    CHECK(contains(read.issues[4].reason, "1 <playcount> value(s) that are not a count (decimal digits): kept as "
                                          "written, read as 0; the first, \"-1\", at line 39"));
    CHECK(contains(read.issues[5].reason, "<lastplayed>"));
}

TEST_CASE("esde/gamelist: unknown elements kept, repeated and stray ones ignored as ES-DE ignores them",
          "[esde][gamelist]") {
    const Scratch scratch;
    const Gamelist read = scratch.read("<?xml version=\"1.0\"?>\n"
                                       "<gameList>\n"
                                       "\t<provider><System>Game Boy</System></provider>\n"         // 3
                                       "\t<game id=\"7\" source=\"Skraper\">\n"                      // 4
                                       "\t\t<path>./a.gb</path>\n"
                                       "\t\t<image>./images/a.png</image>\n"
                                       "\t\t<name>First</name>\n"
                                       "\t\t<name>Second</name>\n"                                   // 8
                                       "\t\t<region>eu</region>\n"
                                       "\t\t<region>us</region>\n"
                                       "\t\t<scrap name=\"ScreenScraper\" date=\"2023-01-02\"/>\n"
                                       "\t\t<controller>gamepad</controller>\n"
                                       "\t\t<collectionsortname>A, collection</collectionsortname>\n"
                                       "\t\t<nogamecount>true</nogamecount>\n"
                                       "\t\t<nested><inner>deep</inner>tail</nested>\n"
                                       "\t\t<desc><![CDATA[Raw <b>&amp;</b>]]></desc>\n"
                                       "\t\t<path>./b.gb</path>\n"                                   // 17
                                       "\t\t<rating>0.5</rating>\n"
                                       "\t\t<rating>0.9</rating>\n"                                  // 19
                                       "\t\tstray text\n"
                                       "\t</game>\n"
                                       "\t<folder>\n"                                                // 22
                                       "\t\t<path>./F</path>\n"
                                       "\t\t<folderlink>./F/a.gb</folderlink>\n"
                                       "\t\t<name>First folder name</name>\n"
                                       "\t\t<name>Second</name>\n"                                   // 26
                                       "\t</folder>\n"
                                       "</gameList>\n"
                                       "<extra/>\n"                                                  // 29
                                       "<gameList><game><path>./c.gb</path></game></gameList>\n");  // 30

    // The first <gameList> only.
    CHECK(pathsOf(read) == std::vector<std::string>{"./a.gb", "./F"});
    const GamelistEntry& a = entryAt(read, "./a.gb");
    CHECK(a.line == 4);
    CHECK(a.metadata.name == "First");
    CHECK(a.metadata.description == "Raw <b>&amp;</b>"); // CDATA as written, never decoded
    CHECK(a.metadata.rating == rv::catalog::Rating{5});
    CHECK(a.raw.originalForms == std::vector<RawElement>{{"rating", "0.5"}});
    // Unmodelled elements in document order, repeated ones each kept; their attributes and children are not.
    CHECK(a.raw.unknownElements == std::vector<RawElement>{{"image", "./images/a.png"},
                                                           {"region", "eu"},
                                                           {"region", "us"},
                                                           {"scrap", ""},
                                                           {"controller", "gamepad"},
                                                           {"collectionsortname", "A, collection"},
                                                           {"nogamecount", "true"},
                                                           {"nested", "tail"}});
    const GamelistEntry& folder = entryAt(read, "./F");
    CHECK(folder.metadata.name == "First folder name");
    CHECK(folder.raw.unknownElements == std::vector<RawElement>{{"folderlink", "./F/a.gb"}});

    const std::vector<std::string> expected{
        "1 <provider> element(s) inside <gameList> besides <game> and <folder>, which ES-DE ignores; the first at "
        "line 3",
        "2 repeated <name> element(s) in an entry, which ES-DE ignores (it reads the first); the first at line 8",
        "1 repeated <path> element(s) in an entry, which ES-DE ignores (it reads the first); the first at line 17",
        "1 repeated <rating> element(s) in an entry, which ES-DE ignores (it reads the first); the first at line 19",
        "1 top-level <extra> element(s) besides the first <gameList> and the first <alternativeEmulator>, which ES-DE "
        "ignores; the first at line 29",
        "1 top-level <gameList> element(s) besides the first <gameList> and the first <alternativeEmulator>, which "
        "ES-DE ignores; the first at line 30",
    };
    std::vector<std::string> reasons;
    for (const rv::catalog::Issue& issue : read.issues) {
        CHECK(issue.code == rv::esde::kGamelistIssueIgnoredElement);
        CHECK(issue.severity == IssueSeverity::Info);
        reasons.push_back(issue.reason);
    }
    CHECK(reasons == expected);
}

TEST_CASE("esde/gamelist: encodings, byte order marks and line ends", "[esde][gamelist]") {
    const Scratch scratch;

    SECTION("a UTF-8 byte order mark and CRLF line ends") {
        const Gamelist read = scratch.read("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                                           "<gameList>\r\n"
                                           "\t<game>\r\n"
                                           "\t\t<path>./Pok\xC3\xA9mon.gb</path>\r\n"
                                           "\t\t<desc>Line one.\r\nLine two.</desc>\r\n"
                                           "\t</game>\r\n"
                                           "\t<game>\r\n"
                                           "\t\t<path>./../x.gb</path>\r\n"
                                           "\t</game>\r\n"
                                           "</gameList>\r\n");
        const GamelistEntry& pokemon = entryAt(read, "./Pok\xC3\xA9mon.gb");
        CHECK(pokemon.line == 3);
        CHECK(relativePathOf(pokemon) == "Pok\xC3\xA9mon.gb");
        CHECK(pokemon.metadata.description == "Line one.\nLine two."); // line ends read as "\n"
        CHECK(entryAt(read, "./../x.gb").line == 8);                    // after the description's two lines
        REQUIRE(read.issues.size() == 1);
        CHECK(contains(read.issues[0].reason, "line 8"));
    }
    SECTION("bytes that are not UTF-8 read as Windows-1252 characters, reported") {
        const Gamelist read = scratch.read(gameList("<game><path>./Caf\xE9.gb</path><name>Caf\xE9 \x93Racer\x94</name>"
                                                    "</game>\n"));
        const GamelistEntry& cafe = entryAt(read, "./Caf\xC3\xA9.gb");
        CHECK(relativePathOf(cafe) == "Caf\xC3\xA9.gb");
        CHECK(cafe.metadata.name == "Caf\xC3\xA9 \xE2\x80\x9CRacer\xE2\x80\x9D");
        REQUIRE(read.issues.size() == 1);
        CHECK(read.issues[0].code == rv::esde::kGamelistIssueReadLeniently);
        CHECK(read.issues[0].severity == IssueSeverity::Warning);
        CHECK(contains(read.issues[0].reason, "4 byte(s) that are not UTF-8 read as Windows-1252 characters, the first "
                                              "at line 3"));
    }
    SECTION("UTF-16 with its byte order mark, characters beyond the BMP") {
        const Gamelist read = scratch.read(
            utf16le(U"<?xml version=\"1.0\"?>\r\n<gameList>\r\n<game><path>./Pok\u00E9mon \U0001F3AE.gb</path>"
                    U"<name>Pok\u00E9mon</name></game>\r\n<game><path>./../y.gb</path></game>\r\n</gameList>\r\n"));
        const GamelistEntry& pokemon = entryAt(read, "./Pok\xC3\xA9mon \xF0\x9F\x8E\xAE.gb");
        CHECK(pokemon.line == 3);
        CHECK(pokemon.metadata.name == "Pok\xC3\xA9mon");
        REQUIRE(read.issues.size() == 1);
        CHECK(contains(read.issues[0].reason, "line 4"));
    }
    SECTION("character references, and references to no character") {
        const Gamelist read = scratch.read(
            gameList("<game><path>./&#x41;&#66;.gb</path><name>&#xE9;t&#xE9; &#0; &amp;amp;</name></game>\n"));
        const GamelistEntry& ab = entryAt(read, "./AB.gb");
        CHECK(ab.metadata.name == "\xC3\xA9t\xC3\xA9 \xEF\xBF\xBD &amp;");
        REQUIRE(read.issues.size() == 1);
        CHECK(read.issues[0].code == rv::esde::kGamelistIssueReadLeniently);
        CHECK(contains(read.issues[0].reason, "1 character reference(s) to no character"));
    }
}

TEST_CASE("esde/gamelist: a huge field near the size cap", "[esde][gamelist]") {
    static_assert(rv::esde::kMaxGamelistBytes == 64ull * 1024ull * 1024ull);
    const Scratch scratch;
    constexpr std::uintmax_t kCap = 4ull * 1024ull * 1024ull;
    const std::string head = "<?xml version=\"1.0\"?>\n<gameList>\n<game><path>./Big.gb</path><desc>";
    const std::string tail = "</desc><name>After</name></game>\n</gameList>\n";
    const std::string line = "Caf\xC3\xA9 au lait, line after line.\n";
    std::string body;
    while (head.size() + body.size() + line.size() + tail.size() <= kCap) {
        body += line;
    }
    body.append(kCap - head.size() - body.size() - tail.size(), 'x'); // the file is exactly the cap

    const fs::path file = scratch.write(head + body + tail);
    REQUIRE(fs::file_size(file) == kCap);
    const Gamelist read = scratch.readFile(file, kCap);
    const GamelistEntry& big = entryAt(read, "./Big.gb");
    CHECK(big.metadata.description.size() == body.size());
    CHECK(big.metadata.description == body);
    CHECK(big.metadata.name == "After");
    CHECK(read.issues.empty());
    // One byte more is refused before anything is parsed.
    CHECK(scratch.failureOfFile(scratch.write(head + body + "x" + tail, "over.xml"), kCap).code ==
          rv::ErrorCode::OutOfRange);
}

TEST_CASE("esde/gamelist: what a broken file makes is bounded", "[esde][gamelist]") {
    const Scratch scratch;
    constexpr std::size_t kListed = rv::esde::kGamelistIssuesListedPerCode;
    static_assert(kListed == 100);
    const auto summary = [](std::size_t more, std::size_t lastLine) {
        return std::to_string(more) + " more issue(s) of this kind, not listed one by one after the first 100; the "
                                      "last at line " +
               std::to_string(lastLine);
    };

    SECTION("entries without a path: the first ones listed, the others counted") {
        // About 1 MiB of empty entries, one per line from line 3: an issue each, unbounded.
        constexpr std::size_t kEntries = 150000;
        std::string body;
        for (std::size_t i = 0; i < kEntries; ++i) {
            body += "<game/>\n";
        }
        const Gamelist read = scratch.read(gameList(body));
        CHECK(read.entries.empty());
        REQUIRE(read.issues.size() == kListed + 1);
        for (const rv::catalog::Issue& issue : read.issues) {
            CHECK(issue.code == rv::esde::kGamelistIssueEntryWithoutPath);
            CHECK(issue.severity == IssueSeverity::Warning);
        }
        CHECK(read.issues[kListed - 1].reason ==
              "the <game> entry at line 102 has no <path>: skipped, as ES-DE skips it");
        CHECK(read.issues.back().reason == summary(kEntries - kListed, kEntries + 2));
    }
    SECTION("one entry written again and again: kept once, in the place of the first") {
        std::string body;
        for (int i = 0; i < 300; ++i) {
            body += "<game><path>./a.gb</path><name>N" + std::to_string(i) + "</name></game>\n";
        }
        const Gamelist read = scratch.read(gameList(body));
        REQUIRE(read.entries.size() == 1);
        CHECK(read.entries[0].metadata.name == "N299");
        CHECK(read.entries[0].line == 302);
        REQUIRE(read.issues.size() == kListed + 1);
        CHECK(read.issues[0].reason == "the <game> entry at line 3 (\"./a.gb\") is superseded by the <game> entry at "
                                       "line 4 (\"./a.gb\"), which names the same path: the last one counts, as in "
                                       "ES-DE");
        CHECK(read.issues.back().code == rv::esde::kGamelistIssueDuplicatePath);
        CHECK(read.issues.back().reason == summary(299 - kListed, 302));
    }
    SECTION("entries outside the folder: every one kept, their issues listed then counted") {
        std::string body;
        for (int i = 0; i < 150; ++i) {
            body += "<game><path>./../other/" + std::to_string(i) + ".gb</path></game>\n";
        }
        const Gamelist read = scratch.read(gameList(body));
        REQUIRE(read.entries.size() == 150);
        CHECK(std::all_of(read.entries.begin(), read.entries.end(),
                          [](const GamelistEntry& entry) { return entry.outsideSystemFolder(); }));
        REQUIRE(read.issues.size() == kListed + 1);
        CHECK(read.issues.back().code == rv::esde::kGamelistIssuePathOutsideFolder);
        CHECK(read.issues.back().reason == summary(50, 152));
    }
    SECTION("ignored elements of many names: counted apart up to the limit, then together") {
        static_assert(rv::esde::kGamelistNamesCountedPerKind == 16);
        std::string content = "<?xml version=\"1.0\"?>\n<alternativeEmulator>\n<label>A</label>\n"; // lines 1-3
        for (int i = 0; i < 40; ++i) {
            content += "<a" + std::to_string(i) + "/>\n"; // lines 4-43
        }
        content += "</alternativeEmulator>\n<gameList>\n"; // lines 44-45
        for (int i = 0; i < 40; ++i) {
            content += "<x" + std::to_string(i) + "/>\n"; // lines 46-85
        }
        content += "<x0/>\n</gameList>\n"; // lines 86-87: a name counted apart, once more
        for (int i = 0; i < 40; ++i) {
            content += "<t" + std::to_string(i) + "/>\n"; // lines 88-127
        }
        const Gamelist read = scratch.read(content);
        CHECK(read.alternativeEmulator == "A");
        REQUIRE(read.issues.size() == 3 * 17);
        for (const rv::catalog::Issue& issue : read.issues) {
            CHECK(issue.code == rv::esde::kGamelistIssueIgnoredElement);
            CHECK(issue.severity == IssueSeverity::Info);
        }
        CHECK(read.issues[15].reason == "1 <a15> element(s) inside <alternativeEmulator> besides its first <label>, "
                                        "which ES-DE ignores; the first at line 19");
        CHECK(read.issues[16].reason == "24 element(s) of other names inside <alternativeEmulator> besides its first "
                                        "<label>, which ES-DE ignores; the first, <a16>, at line 20");
        CHECK(read.issues[17].reason == "2 <x0> element(s) inside <gameList> besides <game> and <folder>, which ES-DE "
                                        "ignores; the first at line 46");
        CHECK(read.issues[33].reason == "24 element(s) of other names inside <gameList> besides <game> and <folder>, "
                                        "which ES-DE ignores; the first, <x16>, at line 62");
        CHECK(read.issues[50].reason == "24 top-level element(s) of other names besides the first <gameList> and the "
                                        "first <alternativeEmulator>, which ES-DE ignores; the first, <t16>, at line "
                                        "104");
    }
    SECTION("unknown elements of an entry beyond the limit") {
        static_assert(rv::esde::kGamelistUnknownElementsPerEntry == 256);
        std::string body = "<game>\n<path>./u.gb</path>\n"; // lines 3-4
        for (int i = 0; i < 300; ++i) {
            body += "<u" + std::to_string(i) + ">t</u" + std::to_string(i) + ">\n"; // lines 5-304
        }
        body += "<name>After</name>\n</game>\n";
        const Gamelist read = scratch.read(gameList(body));
        const GamelistEntry& entry = entryAt(read, "./u.gb");
        CHECK(entry.metadata.name == "After");
        REQUIRE(entry.raw.unknownElements.size() == 256);
        CHECK(entry.raw.unknownElements.front() == RawElement{"u0", "t"});
        CHECK(entry.raw.unknownElements.back() == RawElement{"u255", "t"});
        REQUIRE(read.issues.size() == 1);
        CHECK(read.issues[0].code == rv::esde::kGamelistIssueIgnoredElement);
        CHECK(read.issues[0].reason == "44 unknown element(s) of an entry beyond its first 256, which ES-DE ignores "
                                       "and Retroverse does not keep; the first at line 261");
    }
}

TEST_CASE("esde/gamelist: a path longer than any path Windows can name is skipped before it is resolved",
          "[esde][gamelist]") {
    static_assert(rv::esde::kGamelistMaxPathUtf16Units == 32767);
    const Scratch scratch;
    // Counted in UTF-16 code units: one per character, two beyond U+FFFF.
    const std::string atLimit = "./" + std::string(32767 - 5, 'a') + ".gb";
    std::string euros = "./"; // three bytes, one code unit each
    std::string pads = "./";  // four bytes, two code units each
    for (int i = 0; i < 32767 - 5; ++i) {
        euros += "\xE2\x82\xAC";
    }
    for (int i = 0; i < (32767 - 5) / 2; ++i) {
        pads += "\xF0\x9F\x8E\xAE";
    }
    euros += ".gb";
    pads += ".gb";
    std::string deep = "./"; // as many elements as a path holds: the costliest path to resolve
    while (deep.size() < 32767) {
        deep += "a/";
    }
    deep += "z.gb";

    const Gamelist read = scratch.read(gameList("<game><path>" + atLimit + "</path><name>A</name></game>\n" +    // 3
                                                "<game><path>" + atLimit + "a</path></game>\n" +                 // 4
                                                "<game><path>" + euros + "</path><name>E</name></game>\n" +      // 5
                                                "<game><path>./\xE2\x82\xAC" + euros.substr(2) + "</path></game>\n" +
                                                "<game><path>" + pads + "</path><name>P</name></game>\n" +       // 7
                                                "<game><path>" + pads + "x</path></game>\n" +                    // 8
                                                "<game><path>" + deep + "</path></game>\n"));                    // 9
    CHECK(pathsOf(read) == std::vector<std::string>{atLimit, euros, pads});
    CHECK(relativePathOf(entryAt(read, atLimit)) == atLimit.substr(2));
    const auto malformed = issuesWith(read, rv::esde::kGamelistIssuePathMalformed);
    REQUIRE(malformed.size() == 4);
    CHECK(contains(malformed[0].reason, "the <game> entry at line 4 is skipped: its path \"./aaaa"));
    CHECK(contains(malformed[0].reason, "...\" is longer than any path Windows can name (32768 UTF-16 code units, at "
                                        "most 32767)"));
    CHECK(contains(malformed[1].reason, "line 6"));
    CHECK(contains(malformed[1].reason, "(32768 UTF-16 code units"));
    CHECK(contains(malformed[2].reason, "line 8"));
    CHECK(contains(malformed[2].reason, "(32768 UTF-16 code units"));
    CHECK(contains(malformed[3].reason, "line 9"));
    CHECK(malformed[3].reason.size() < 400);
}

TEST_CASE("esde/gamelist: what was read leniently is an issue, logged at debug level only", "[esde][gamelist]") {
    const Scratch scratch;
    const fs::path file = scratch.write(gameList("<game><path>./Caf\xE9.gb</path><name>&#0;</name></game>\n"));
    const std::string path = rv::core::pathToUtf8(file);
    const rv::test::LogCapture log;
    spdlog::default_logger()->set_level(spdlog::level::trace); // the capture's own logger, every level
    const Gamelist read = scratch.readFile(file);

    REQUIRE(read.issues.size() == 2);
    CHECK(read.issues[0].code == rv::esde::kGamelistIssueReadLeniently);
    CHECK(read.issues[1].code == rv::esde::kGamelistIssueReadLeniently);
    // The file's path is the user's (C:\Users\<name>\ES-DE\gamelists\...): never logged above debug level.
    CHECK(log.count("debug " + path + ": ") == 2); // each warning, at debug level
    const std::string text = log.text();
    for (const std::string_view line : rv::core::split(text, '\n')) {
        INFO(line);
        CHECK((line.empty() || line.starts_with("debug ") || line.starts_with("trace ")));
    }
}

TEST_CASE("esde/gamelist: control characters", "[esde][gamelist]") {
    const Scratch scratch;
    const Gamelist read = scratch.read(gameList("<game><path>./Ctrl.gb</path><name>Tab\there&#1;</name>"
                                                "<desc>a\x01"
                                                "b\x1F"
                                                "c</desc></game>\n"                            // 3
                                                "<game><path>./a&#9;b.gb</path></game>\n"      // 4
                                                "<game><path>./a\x1F"
                                                "b.gb</path></game>\n"                         // 5
                                                "<game><path>./a&#127;b.gb</path></game>\n")); // 6

    // In text, kept as written (displayText() makes a tab a space where a value is shown on one line).
    const GamelistEntry& ctrl = entryAt(read, "./Ctrl.gb");
    CHECK(ctrl.metadata.name == "Tab\there\x01");
    CHECK(ctrl.metadata.description ==
          "a\x01"
          "b\x1F"
          "c");
    // In a path, refused (NTFS and exFAT refuse them in names), except DEL, which they allow.
    CHECK(relativePathOf(entryAt(read, "./a\x7F"
                                       "b.gb")) == "a\x7F"
                                                   "b.gb");
    CHECK(read.entries.size() == 2);
    const auto malformed = issuesWith(read, rv::esde::kGamelistIssuePathMalformed);
    REQUIRE(malformed.size() == 2);
    CHECK(contains(malformed[0].reason, "line 4"));
    CHECK(contains(malformed[0].reason, "\"./a\\x09b.gb\"")); // quoted printably
    CHECK(contains(malformed[0].reason, "control character"));
    CHECK(contains(malformed[1].reason, "\"./a\\x1Fb.gb\""));
}

TEST_CASE("esde/gamelist: nesting within the loader's limit and beyond it", "[esde][gamelist]") {
    const Scratch scratch;
    const auto nested = [](std::size_t levels) {
        std::string text = "<game><path>./Deep.gb</path>";
        for (std::size_t i = 0; i < levels; ++i) {
            text += "<level>";
        }
        text += "bottom";
        for (std::size_t i = 0; i < levels; ++i) {
            text += "</level>";
        }
        return text + "</game>\n";
    };
    // <gameList>, <game> and 62 levels: 64, the deepest the loader reads. An unknown element's children are not kept.
    const Gamelist read = scratch.read(gameList(nested(62)));
    CHECK(entryAt(read, "./Deep.gb").raw.unknownElements == std::vector<RawElement>{{"level", ""}});
    // Deeper: a failure, never a crash.
    for (const std::size_t levels : {std::size_t{63}, std::size_t{100000}}) {
        const rv::Error error = scratch.failure(gameList(nested(levels)));
        CHECK(error.code == rv::ErrorCode::ParseError);
        CHECK(contains(error.message, "nested deeper than 64 levels"));
    }
}

TEST_CASE("esde/gamelist: entry kinds and long messages", "[esde][gamelist]") {
    CHECK(rv::esde::toString(GamelistEntryKind::Game) == "game");
    CHECK(rv::esde::toString(GamelistEntryKind::Folder) == "folder");

    // A message quotes at most 120 bytes of a path, cut at a character boundary.
    const Scratch scratch;
    std::string longPath = "./..";
    for (int i = 0; i < 100; ++i) {
        longPath += "/\xC3\xA9l\xC3\xA9ment";
    }
    const Gamelist read = scratch.read(gameList("<game><path>" + longPath + ".gb</path></game>\n"));
    REQUIRE(read.issues.size() == 1);
    const std::string& reason = read.issues[0].reason;
    CHECK(reason.size() < 400);
    CHECK(rv::core::isValidUtf8(reason));
    CHECK(contains(reason, "...\""));
}
