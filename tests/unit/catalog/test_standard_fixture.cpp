// The standard ES-DE fixture (tests/fixtures/esde/standard, I2 step 1): it materializes; its files read as its README
// says through rv_esde's XML loader and rv_core's walker; and a simplified reading of ES-DE's rules over it (extensions
// compared with their case, hidden files and <hidden> entries as the settings say, cue sheets grouping their tracks,
// gamelist paths resolved against their system's folder and matched to files by lookup key) gives the counts the README
// promises the later steps, for the committed settings and for their variants. The later steps implement the real
// rules and assert the same counts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/lookup_key.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/xml_outline.hpp>
#include <rv/launcher/launch_request.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::esde::XmlElementOutline;
using rv::esde::XmlOutline;

namespace {

constexpr std::uintmax_t kMaxXmlBytes = 1024 * 1024;
constexpr std::string_view kSettings = "ES-DE/settings/es_settings.xml";

rv::test::Fixture materializeStandard() {
    auto materialized = rv::test::Fixture::materialize("esde/standard");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    return std::move(*materialized);
}

XmlOutline outline(const fs::path& file) {
    auto read = rv::esde::outlineXmlFile(file, kMaxXmlBytes);
    INFO(rv::core::pathToUtf8(file) << ": " << (read ? "read" : read.error().toString()));
    REQUIRE(read.has_value());
    CHECK(read->warnings.empty());
    CHECK(read->encoding == "UTF-8");
    return std::move(*read);
}

std::string attribute(const XmlElementOutline& element, std::string_view name) {
    for (const auto& [key, value] : element.attributes) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

/// The text of the child element `name`, or nothing.
std::optional<std::string> childText(const XmlElementOutline& element, std::string_view name) {
    for (const XmlElementOutline& child : element.children) {
        if (child.name == name) {
            return child.text;
        }
    }
    return std::nullopt;
}

/// The value of the es_settings.xml setting `name`.
std::string setting(const XmlOutline& settings, std::string_view name) {
    const auto found =
        std::find_if(settings.elements.begin(), settings.elements.end(),
                     [&](const XmlElementOutline& element) { return attribute(element, "name") == name; });
    INFO("the setting " << name);
    REQUIRE(found != settings.elements.end());
    return attribute(*found, "value");
}

/// Turns the boolean setting `name` of a materialized fixture off.
void turnOff(const rv::test::Fixture& fixture, std::string_view name) {
    const std::string on = "name=\"" + std::string(name) + "\" value=\"true\"";
    const std::string off = "name=\"" + std::string(name) + "\" value=\"false\"";
    const rv::Status replaced = fixture.replaceText(kSettings, on, off);
    INFO((replaced ? "replaced" : replaced.error().toString()));
    REQUIRE(replaced.has_value());
}

/// The game entry of a gamelist whose <path> is `path`.
const XmlElementOutline& gamelistEntry(const XmlElementOutline& gameList, std::string_view path) {
    const auto found = std::find_if(gameList.children.begin(), gameList.children.end(),
                                    [&](const XmlElementOutline& entry) { return childText(entry, "path") == path; });
    INFO("the gamelist entry " << path);
    REQUIRE(found != gameList.children.end());
    return *found;
}

/// The last element of a relative path.
std::string_view nameOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// ES-DE's extension of the file a relative path names: from the last dot of its name, "." for none.
std::string extensionOf(std::string_view path) {
    const std::string_view name = nameOf(path);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string(".") : std::string(name.substr(dot));
}

/// The extension list of es_systems.xml split on blanks and commas, verbatim.
std::set<std::string> extensionsOf(std::string_view list) {
    std::set<std::string> extensions;
    std::string current;
    for (const char c : list) {
        if (c == ' ' || c == '\t' || c == '\n' || c == ',') {
            if (!current.empty()) {
                extensions.insert(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        extensions.insert(current);
    }
    return extensions;
}

/// The files a cue sheet's FILE lines name.
std::vector<std::string> cueFiles(std::string_view sheet) {
    std::vector<std::string> files;
    for (const std::string_view line : rv::core::split(sheet, '\n')) {
        const std::string_view trimmed = rv::core::trim(line);
        if (trimmed.starts_with("FILE \"")) {
            const std::size_t end = trimmed.find('"', 6);
            files.emplace_back(trimmed.substr(6, end - 6));
        }
    }
    return files;
}

std::string keyOf(std::string_view relativePath) {
    auto key = rv::catalog::makeLookupKey(relativePath);
    INFO(relativePath);
    REQUIRE(key.has_value());
    return key->key();
}

/// One system's counts, as the README's tables give them.
struct Counts {
    int entries = 0;
    int games = 0;
    int missing = 0;
    int notConfigured = 0; ///< not a game: extension not configured
    int excluded = 0;      ///< not a game: excluded by a setting
    int members = 0;
    int folders = 0;
    int outside = 0;
    int unscraped = 0;

    bool operator==(const Counts&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Counts& c) {
    return out << "{entries " << c.entries << ", games " << c.games << ", missing " << c.missing
               << ", extension not configured " << c.notConfigured << ", excluded by a setting " << c.excluded
               << ", members " << c.members << ", folders " << c.folders << ", outside " << c.outside << ", unscraped "
               << c.unscraped << "}";
}

/// What the simplified reading finds: the counts per system, and how many games ES-DE itself shows.
struct Reading {
    std::map<std::string, Counts> systems;
    int esdeShows = 0;
};

/// The simplified reading of ES-DE's rules over a materialized standard fixture, with its current settings.
Reading readWithEsdeRules(const rv::test::Fixture& fixture) {
    const XmlOutline settings = outline(fixture.path(kSettings));
    const bool showHiddenFiles = setting(settings, "ShowHiddenFiles") == "true";
    const bool showHiddenGames = setting(settings, "ShowHiddenGames") == "true";
    const XmlOutline systems = outline(fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml"));
    REQUIRE(systems.elements.size() == 1);
    const auto listed = rv::core::listTree(fixture.path("roms"));
    REQUIRE(listed.has_value());

    Reading reading;
    for (const XmlElementOutline& systemElement : systems.elements[0].children) {
        const std::string name = childText(systemElement, "name").value();
        const std::set<std::string> extensions = extensionsOf(childText(systemElement, "extension").value());
        const std::string prefix = name + "/";
        Counts& c = reading.systems[name];

        // The system's files by key; the ones ES-DE lists (a configured extension, compared with its case), less the
        // hidden ones while ShowHiddenFiles is off, which are entries excluded by that setting.
        std::map<std::string, std::string> fileByKey; // key -> the relative path as listed
        std::set<std::string> listedGames;            // relative paths
        for (const rv::core::TreeEntry& entry : *listed) {
            if (entry.kind != rv::core::TreeEntryKind::File || !entry.relativePath.starts_with(prefix)) {
                continue;
            }
            const std::string relative = entry.relativePath.substr(prefix.size());
            fileByKey[keyOf(relative)] = relative;
            if (!extensions.contains(extensionOf(relative))) {
                continue;
            }
            if (!showHiddenFiles && (entry.hidden || nameOf(relative).starts_with('.'))) {
                ++c.excluded;
            } else {
                listedGames.insert(relative);
            }
        }

        // Gamelist entries, matched to the files by key: a path that differs from its file in case only still names
        // it (ADR-0008 §3), where ES-DE shows a second entry.
        const XmlOutline gamelist = outline(fixture.path("ES-DE/gamelists/" + name + "/gamelist.xml"));
        const XmlElementOutline& gameList = gamelist.elements.back();
        REQUIRE(gameList.name == "gameList");
        std::set<std::string> scraped;
        int caseOnly = 0;
        for (const XmlElementOutline& entry : gameList.children) {
            const std::string path = childText(entry, "path").value();
            INFO(name << " " << path);
            const auto relative = rv::core::resolveRelativePath(path, fixture.path("roms/" + name));
            if (!relative) {
                CHECK(relative.error().code == rv::ErrorCode::OutOfRange);
                ++c.outside;
                continue;
            }
            if (entry.name == "folder") {
                CHECK(fs::is_directory(fixture.path("roms/" + prefix + *relative)));
                ++c.folders;
                continue;
            }
            const auto file = fileByKey.find(keyOf(*relative));
            if (file == fileByKey.end()) {
                ++c.missing;
                continue;
            }
            const std::string& onDisk = file->second;
            if (!extensions.contains(extensionOf(onDisk))) {
                ++c.notConfigured;
                continue;
            }
            scraped.insert(onDisk);
            if (!listedGames.contains(onDisk)) {
                continue; // excluded already: a hidden file while ShowHiddenFiles is off
            }
            if (!showHiddenGames && childText(entry, "hidden") == "true") {
                listedGames.erase(onDisk); // ES-DE drops the entry when it loads the gamelist
                ++c.excluded;
                continue;
            }
            if (onDisk != *relative) {
                ++caseOnly;
            }
        }

        // Disc members: the files a listed cue sheet names that are listed themselves.
        std::set<std::string> members;
        for (const std::string& game : listedGames) {
            if (extensionOf(game) == ".cue") {
                const std::string sheet = rv::core::readTextFile(fixture.path("roms/" + prefix + game)).value();
                for (const std::string& track : cueFiles(sheet)) {
                    if (listedGames.contains(track)) {
                        members.insert(track);
                    }
                }
            }
        }
        c.members = static_cast<int>(members.size());
        c.games = static_cast<int>(listedGames.size() - members.size());
        for (const std::string& game : listedGames) {
            if (!members.contains(game) && !scraped.contains(game)) {
                ++c.unscraped;
            }
        }
        c.entries = c.games + c.members + c.missing + c.notConfigured + c.excluded + c.folders + c.outside;
        reading.esdeShows += static_cast<int>(listedGames.size()) + caseOnly;
    }
    return reading;
}

} // namespace

TEST_CASE("catalog/standard fixture: the parts of an ES-DE installation", "[catalog][fixture]") {
    const rv::test::Fixture fixture = materializeStandard();
    const std::string rootText = rv::core::pathToUtf8(fixture.root());

    // The home at its default place under the fixture's home folder, with an empty custom_systems.
    CHECK(fs::is_directory(fixture.path("ES-DE/gamelists")));
    CHECK(fs::is_directory(fixture.path("ES-DE/custom_systems")));
    CHECK(fs::is_empty(fixture.path("ES-DE/custom_systems")));
    // The install: a placeholder ES-DE.exe, and the copy of the fake emulator where the find rules look, a program
    // the launcher accepts.
    CHECK(fs::file_size(fixture.path("Programs/ES-DE/ES-DE.exe")) == 4096);
    const std::string fakeEmulatorHash =
        rv::core::sha256Hex(rv::core::readTextFile(rv::test::testProgramsDir() / "rv_fake_emulator.exe").value());
    const fs::path emulator = fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");
    REQUIRE(fs::exists(emulator));
    CHECK(rv::core::sha256Hex(rv::core::readTextFile(emulator).value()) == fakeEmulatorHash);
    rv::launcher::LaunchRequest request;
    request.executable = emulator;
    const rv::Status valid = rv::launcher::validateLaunchRequest(request);
    INFO((valid ? "valid" : valid.error().toString()));
    CHECK(valid.has_value());
    CHECK(fs::file_size(fixture.path("Programs/ES-DE/Emulators/FakeEmulator/cores/fake_gb_libretro.dll")) == 2048);
    CHECK(fs::exists(fixture.path("Programs/ES-DE/Emulators/FakeEmulator/cores/fake_psx_libretro.dll")));
    CHECK_FALSE(fs::exists(fixture.path("Programs/ES-DE/Emulators/FakeEmulator/cores/fake_missing_libretro.dll")));
    // The emulator under `~`: ES-DE's home folder is the fixture root, the parent of the ES-DE home; a `~` read as the
    // ES-DE home would find nothing.
    const fs::path homeEmulator = fixture.path("Emulators/HomeEmulator/home-emulator.exe");
    REQUIRE(fs::exists(homeEmulator));
    CHECK(rv::core::sha256Hex(rv::core::readTextFile(homeEmulator).value()) == fakeEmulatorHash);
    CHECK_FALSE(fs::exists(fixture.path("ES-DE/Emulators/HomeEmulator/home-emulator.exe")));

    // Settings: sibling elements without a root, the two folders under the fixture, the fake credentials.
    const XmlOutline settings = outline(fixture.path(kSettings));
    CHECK(settings.elements.size() == 13);
    CHECK(setting(settings, "ROMDirectory") == rootText + "\\roms");
    CHECK(setting(settings, "MediaDirectory") == rootText + "/media");
    CHECK(setting(settings, "ShowHiddenFiles") == "true");
    CHECK(setting(settings, "ShowHiddenGames") == "true");
    CHECK(setting(settings, "ParseGamelistOnly") == "false");
    CHECK(setting(settings, "LegacyGamelistFileLocation") == "false");
    CHECK(setting(settings, "AlternativeEmulatorPerGame") == "true");
    CHECK(setting(settings, "ScraperUsernameScreenScraper") == "fixture-scraper-user-Q7x4");
    CHECK(setting(settings, "ScraperPasswordScreenScraper") == "fixture-scraper-password-K9w2");

    // Systems and find rules.
    const XmlOutline systems = outline(fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml"));
    REQUIRE(systems.elements.size() == 1);
    std::vector<std::string> names;
    for (const XmlElementOutline& system : systems.elements[0].children) {
        names.push_back(childText(system, "name").value_or(""));
        CHECK(childText(system, "path") == "%ROMPATH%\\" + names.back());
    }
    CHECK(names == std::vector<std::string>{"gb", "snes", "psx"});
    std::vector<std::string> gbCommands;
    for (const XmlElementOutline& child : systems.elements[0].children[0].children) {
        if (child.name == "command") {
            gbCommands.push_back(attribute(child, "label"));
        }
    }
    CHECK(gbCommands == std::vector<std::string>{"Fake Emulator", "Fake Emulator (Game Folder)",
                                                 "Missing Emulator (Standalone)", "Home Emulator"});
    CHECK(childText(systems.elements[0].children[2], "extension") == ".bin .BIN .chd .CHD .cue CUE .m3u .M3U");
    const XmlOutline rules = outline(fixture.path("Programs/ES-DE/resources/systems/windows/es_find_rules.xml"));
    REQUIRE(rules.elements.size() == 1);
    std::map<std::string, std::vector<std::string>> staticPaths; // emulator -> its static path entries
    for (const XmlElementOutline& ruleSet : rules.elements[0].children) {
        if (ruleSet.name != "emulator") {
            continue;
        }
        for (const XmlElementOutline& rule : ruleSet.children) {
            for (const XmlElementOutline& entry : rule.children) {
                if (attribute(rule, "type") == "staticpath") {
                    staticPaths[attribute(ruleSet, "name")].push_back(entry.text);
                }
            }
        }
    }
    const std::vector<std::string>& fake = staticPaths["FAKE-EMULATOR"];
    CHECK(std::find(fake.begin(), fake.end(), "%ESPATH%\\Emulators\\FakeEmulator\\fake-emulator.exe") != fake.end());
    CHECK(staticPaths["HOME-EMULATOR"] == std::vector<std::string>{"~\\Emulators\\HomeEmulator\\home-emulator.exe"});

    // Gamelists: one with <alternativeEmulator> beside <gameList>; values decoded as ES-DE's pugixml decodes them.
    const XmlOutline gb = outline(fixture.path("ES-DE/gamelists/gb/gamelist.xml"));
    const XmlOutline snes = outline(fixture.path("ES-DE/gamelists/snes/gamelist.xml"));
    const XmlOutline psx = outline(fixture.path("ES-DE/gamelists/psx/gamelist.xml"));
    REQUIRE(gb.elements.size() == 1);
    REQUIRE(snes.elements.size() == 2);
    REQUIRE(psx.elements.size() == 1);
    CHECK(snes.elements[0].name == "alternativeEmulator");
    CHECK(childText(snes.elements[0], "label") == "Fake Emulator (Hidden Console)");
    const XmlElementOutline& tiny = gamelistEntry(gb.elements[0], "./Tiny Tennis (Europe).gb");
    CHECK(childText(tiny, "name") == "Tiny Tennis");
    CHECK(childText(tiny, "desc").value().find("courts.\n\nSingles & doubles") != std::string::npos);
    const XmlElementOutline& cafe = gamelistEntry(gb.elements[0], "./Caf\xC3\xA9 Racer (Europe).gb");
    CHECK(childText(cafe, "name") == "Caf\xC3\xA9 Racer");
    CHECK(childText(cafe, "publisher") == "Northwind Games ");
    CHECK(childText(cafe, "altemulator") == "Fake Emulator (Game Folder)");
    CHECK(childText(gamelistEntry(gb.elements[0], "./Collection A/Pocket Puzzle (World).gb"), "region") == "wor");
    const XmlElementOutline& alpha = gamelistEntry(snes.elements[1], "./Alpha Quest (USA).sfc");
    CHECK(childText(alpha, "desc").value().find("\"Ash &amp; Ember\"") != std::string::npos);
    CHECK(childText(alpha, "developer") == "Lantern Works ");
    CHECK(childText(alpha, "publisher") == "Mirage Soft\n");
    CHECK(childText(gamelistEntry(snes.elements[1], "./delta drift (usa).sfc"), "name") == "Delta Drift");
    CHECK(childText(gamelistEntry(snes.elements[1], "./Sports Vol. 2"), "name") == "Sports, Volume 2");
    const XmlElementOutline& solo = gamelistEntry(psx.elements[0], "./Solo Disc (USA).chd");
    CHECK(childText(solo, "hidden") == "true");
    CHECK(childText(solo, "completed") == "true");
    CHECK(childText(solo, "broken") == "false");
    // The legacy gamelist inside gb's ROM folder names Tiny Tennis otherwise: a reader that took it would show it.
    const XmlOutline legacy = outline(fixture.path("roms/gb/gamelist.xml"));
    REQUIRE(legacy.elements.size() == 1);
    CHECK(childText(gamelistEntry(legacy.elements[0], "./Tiny Tennis (Europe).gb"), "name") == "Legacy Tennis");

    // The ROM tree: names, sizes, times and attributes as files.json gives them.
    const auto listed = rv::core::listTree(fixture.path("roms"));
    REQUIRE(listed.has_value());
    std::map<std::string, rv::core::TreeEntry> files;
    int directories = 0;
    for (const rv::core::TreeEntry& entry : *listed) {
        REQUIRE(entry.kind != rv::core::TreeEntryKind::Error);
        if (entry.kind == rv::core::TreeEntryKind::Directory) {
            ++directories;
        } else {
            files[entry.relativePath] = entry;
        }
    }
    CHECK(files.size() == 23);
    CHECK(directories == 8);
    for (const auto& [path, entry] : files) {
        INFO(path);
        CHECK(entry.hidden == (path == "gb/Hidden Gem (World).gb"));
    }
    const rv::core::TreeEntry& a = files.at("gb/Collection A/Pocket Puzzle (World).gb");
    const rv::core::TreeEntry& b = files.at("gb/Collection B/Pocket Puzzle (World).gb");
    CHECK(a.size == b.size);
    CHECK(a.lastWriteTime == b.lastWriteTime);
    CHECK(std::chrono::clock_cast<std::chrono::system_clock>(a.lastWriteTime) ==
          rv::core::parseUtcIso8601("1996-12-25T05:32:00Z").value());
    CHECK(rv::core::sha256Hex(
              rv::core::readTextFile(fixture.path("roms/gb/Collection A/Pocket Puzzle (World).gb")).value()) ==
          rv::core::sha256Hex(
              rv::core::readTextFile(fixture.path("roms/gb/Collection B/Pocket Puzzle (World).gb")).value()));
    CHECK(files.at("gb/Collection A/Deep/Bottom Dweller (Japan).gbc").size == 196608);
    CHECK(files.at("psx/Twin Tracks (Europe) (Track 1).bin").size == 30 * 2352);
    CHECK(files.contains("snes/Delta Drift (USA).sfc")); // the case as listed, not as the gamelist writes it
    CHECK(files.contains("snes/Sports Vol. 2/Ice Lanes (USA).sfc"));

    // Media: 25 files, mirroring the ROM tree.
    const auto media = rv::core::listTree(fixture.path("media"));
    REQUIRE(media.has_value());
    std::vector<std::string> mediaFiles;
    for (const rv::core::TreeEntry& entry : *media) {
        if (entry.kind == rv::core::TreeEntryKind::File) {
            mediaFiles.push_back(entry.relativePath);
        }
    }
    CHECK(mediaFiles == std::vector<std::string>{
                            "gb/covers/Collection A/Pocket Puzzle (World).png",
                            "gb/covers/Gone Fishing (USA).png",
                            "gb/covers/Tiny Tennis (Europe).jpg",
                            "gb/covers/Tiny Tennis (Europe).png",
                            "gb/screenshots/Tiny Tennis (Europe).jpg",
                            "gb/videos/Caf\xC3\xA9 Racer (Europe).avi",
                            "gb/videos/LOUD CART (USA).mkv",
                            "gb/videos/Tiny Tennis (Europe).mp4",
                            "psx/covers/Twin Tracks (Europe).png",
                            "psx/videos/Twin Tracks (Europe).wmv",
                            "snes/3dboxes/Alpha Quest (USA).png",
                            "snes/backcovers/Alpha Quest (USA).png",
                            "snes/covers/Alpha Quest (USA).png",
                            "snes/covers/Hacks.png",
                            "snes/covers/Sports Vol. 2.png",
                            "snes/custom/Alpha Quest (USA).png",
                            "snes/fanart/Alpha Quest (USA).jpg",
                            "snes/manuals/Alpha Quest (USA).pdf",
                            "snes/marquees/Alpha Quest (USA).png",
                            "snes/miximages/Alpha Quest (USA).png",
                            "snes/physicalmedia/Alpha Quest (USA).png",
                            "snes/screenshots/Alpha Quest (USA).jpg",
                            "snes/titlescreens/Alpha Quest (USA).jpg",
                            "snes/videos/Alpha Quest (USA).mov",
                            "snes/videos/Beta Run (Europe).flv",
                        });

    // The README's counts.
    const Reading reading = readWithEsdeRules(fixture);
    CHECK(reading.esdeShows == 18);
    CHECK(reading.systems.at("gb") == Counts{11, 8, 1, 0, 0, 0, 1, 1, 5});
    CHECK(reading.systems.at("snes") == Counts{8, 5, 0, 1, 0, 0, 2, 0, 2});
    CHECK(reading.systems.at("psx") == Counts{4, 2, 0, 0, 0, 2, 0, 0, 0});
}

TEST_CASE("catalog/standard fixture: the variants of its settings", "[catalog][fixture]") {
    SECTION("ShowHiddenFiles off: the hidden and the leading-dot files are excluded by it") {
        const rv::test::Fixture fixture = materializeStandard();
        turnOff(fixture, "ShowHiddenFiles");
        const Reading reading = readWithEsdeRules(fixture);
        CHECK(reading.esdeShows == 16);
        CHECK(reading.systems.at("gb") == Counts{11, 6, 1, 0, 2, 0, 1, 1, 3});
        CHECK(reading.systems.at("snes") == Counts{8, 5, 0, 1, 0, 0, 2, 0, 2});
        CHECK(reading.systems.at("psx") == Counts{4, 2, 0, 0, 0, 2, 0, 0, 0});
    }
    SECTION("ShowHiddenGames off: the game its gamelist marks hidden is excluded by it") {
        const rv::test::Fixture fixture = materializeStandard();
        turnOff(fixture, "ShowHiddenGames");
        const Reading reading = readWithEsdeRules(fixture);
        CHECK(reading.esdeShows == 17);
        CHECK(reading.systems.at("gb") == Counts{11, 8, 1, 0, 0, 0, 1, 1, 5});
        CHECK(reading.systems.at("snes") == Counts{8, 5, 0, 1, 0, 0, 2, 0, 2});
        CHECK(reading.systems.at("psx") == Counts{4, 1, 0, 0, 1, 2, 0, 0, 0});
    }
    SECTION("both off") {
        const rv::test::Fixture fixture = materializeStandard();
        turnOff(fixture, "ShowHiddenFiles");
        turnOff(fixture, "ShowHiddenGames");
        const Reading reading = readWithEsdeRules(fixture);
        CHECK(reading.esdeShows == 15);
        CHECK(reading.systems.at("gb") == Counts{11, 6, 1, 0, 2, 0, 1, 1, 3});
        CHECK(reading.systems.at("psx") == Counts{4, 1, 0, 0, 1, 2, 0, 0, 0});
    }
}
