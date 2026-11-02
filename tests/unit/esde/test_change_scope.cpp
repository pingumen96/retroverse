// What a changed path means to the library (I7 step 7, ADR-0018 §10): esde::changeScope() over invented folders and
// the standard fixture's configuration, hostile paths included (relative, "..", another casing, \\?\, long, Unicode,
// slashes), an emulator's save and state beside a ROM, a media file that is no medium; esde::watchRoots() and the
// fallback poll's entries.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/text.hpp>
#include <rv/esde/change_scope.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

#include "support/fixture.hpp"

namespace {

using rv::esde::ChangeScope;
using rv::esde::ChangeScopeMap;
using rv::esde::ScopeKind;
namespace fs = std::filesystem;

rv::esde::ScopeSystem scopeSystem(std::string_view name, fs::path folder, std::vector<std::string> extensions) {
    rv::esde::ScopeSystem system;
    system.name = rv::catalog::SystemName::parse(name).value();
    system.romFolder = std::move(folder);
    system.extensions = std::move(extensions);
    return system;
}

/// Invented folders: the ROM root on one drive, a system folder on another, a nested system, the media and the ES-DE
/// home elsewhere.
ChangeScopeMap invented() {
    ChangeScopeMap map;
    map.esdeHome = "X:\\Invented\\ES-DE";
    map.romRoot = fs::path("Y:\\Roms");
    map.mediaRoot = fs::path("X:\\Invented\\Media");
    map.systems = {scopeSystem("gb", "Y:\\Roms\\gb", {".gb", ".GB", ".zip"}),
                   scopeSystem("psx", "Z:\\Discs\\psx", {".chd", ".m3u", ".cue"}),
                   scopeSystem("snes", "Y:\\Roms\\snes", {".sfc", ".SFC", ".zip"}),
                   scopeSystem("snesmsu", "Y:\\Roms\\snes\\msu", {".sfc"}),
                   scopeSystem("unresolved", fs::path(), {".x"})};
    map.systems.back().romFolder.reset();
    return map;
}

/// "<kind>" or "<kind>:<system>", and the reason.
std::string describe(const ChangeScope& scope) {
    std::string text(rv::esde::toString(scope.kind));
    if (scope.kind == ScopeKind::System) {
        text += ":" + scope.system.str();
    }
    return text + " (" + std::string(scope.reason) + ")";
}

std::string scopeOf(const ChangeScopeMap& map, const fs::path& path) {
    return describe(rv::esde::changeScope(map, path));
}

} // namespace

TEST_CASE("esde/change_scope: ROM folders: the system's extensions, a folder, noload.txt; never a save or a state",
          "[esde][change_scope]") {
    const ChangeScopeMap map = invented();
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Zinnia (USA).sfc") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Zinnia (USA).SFC") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Zinnia (USA).Sfc") == "system:snes (rom)"); // case ignored: one refresh more
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Sub Folder\\Deep (Europe).zip") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Collection") == "system:snes (rom)"); // a folder moved in
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Trailing.") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\noload.txt") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes") == "system:snes (rom-folder)");
    // An emulator's files beside the ROMs wake nothing.
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Zinnia (USA).srm") == "none (extension)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\Zinnia (USA).state1") == "none (extension)");
    CHECK(scopeOf(map, "Y:\\Roms\\gb\\saves\\Tiny.sav") == "none (extension)");
    CHECK(scopeOf(map, "Y:\\Roms\\gb\\desktop.ini") == "none (extension)");
    // The deepest ROM folder owns what it holds; a folder outside the ROM root too.
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\msu\\Quest.sfc") == "system:snesmsu (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\msu\\Quest.zip") == "none (extension)");
    CHECK(scopeOf(map, "Z:\\Discs\\psx\\Orbit (Disc 1).chd") == "system:psx (rom)");
    CHECK(scopeOf(map, "Z:\\Discs\\psx\\Orbit.bin") == "none (extension)");
    // The ROM root and what holds system folders: every system; anything else there: nothing.
    CHECK(scopeOf(map, "Y:\\Roms") == "everything (system-folders)");
    CHECK(scopeOf(map, "Y:\\") == "everything (system-folders)");
    CHECK(scopeOf(map, "Y:\\Roms\\readme.txt") == "none (outside)");
    CHECK(scopeOf(map, "Y:\\Roms\\megadrive") == "none (outside)");
    CHECK(scopeOf(map, "Y:\\Elsewhere\\Game.sfc") == "none (outside)");
}

TEST_CASE("esde/change_scope: hostile paths", "[esde][change_scope]") {
    const ChangeScopeMap map = invented();
    CHECK(scopeOf(map, fs::path()) == "none (relative)");
    CHECK(scopeOf(map, "roms\\snes\\Zinnia (USA).sfc") == "none (relative)");
    CHECK(scopeOf(map, "\\Roms\\snes\\Zinnia (USA).sfc") == "none (relative)"); // no drive: not absolute
    CHECK(scopeOf(map, "y:\\ROMS\\SNES\\ZINNIA (USA).SFC") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:/Roms/snes/Zinnia (USA).sfc") == "system:snes (rom)");
    CHECK(scopeOf(map, "\\\\?\\Y:\\Roms\\snes\\Zinnia (USA).sfc") == "system:snes (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\..\\gb\\Tiny (World).gb") == "system:gb (rom)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\..\\..\\Other\\Zinnia.sfc") == "none (outside)");
    CHECK(scopeOf(map, "Y:\\Roms\\snes\\.\\Zinnia.sfc") == "system:snes (rom)");
    CHECK(scopeOf(map, rv::core::pathFromUtf8("Y:\\Roms\\snes\\Pokémon Ëdition (Ç).sfc")) == "system:snes (rom)");
    CHECK(scopeOf(map, rv::core::pathFromUtf8("Y:\\Roms\\SNES\\ÉTÉ.SFC")) == "system:snes (rom)");
    const fs::path deep = fs::path("Y:\\Roms\\snes") / std::string(40, 'd') / std::string(120, 'e') /
                          (std::string(150, 'f') + ".sfc");
    CHECK(deep.native().size() > 260);
    CHECK(scopeOf(map, deep) == "system:snes (rom)");
    // A name of the ROM root spelled like a system's name outside it is nothing.
    CHECK(scopeOf(map, "X:\\snes\\Zinnia (USA).sfc") == "none (outside)");
}

TEST_CASE("esde/change_scope: ES-DE's configuration, gamelists and media", "[esde][change_scope]") {
    const ChangeScopeMap map = invented();
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\settings\\es_settings.xml") == "everything (settings)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\settings\\ES_SETTINGS.XML") == "everything (settings)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\settings\\es_settings.xml.tmp") == "none (settings)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\custom_systems\\es_systems.xml") == "everything (custom-systems)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\custom_systems\\es_find_rules.xml") == "everything (custom-systems)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\custom_systems\\notes.txt") == "none (custom-systems)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\gamelists\\snes\\gamelist.xml") == "system:snes (gamelist)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\gamelists\\SNES") == "system:snes (gamelist)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\gamelists\\snes\\gamelist.xml.bak") == "none (gamelist)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\gamelists\\nes\\gamelist.xml") == "none (unknown-system)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\gamelists") == "everything (gamelist)");
    CHECK(scopeOf(map, "X:\\Invented\\ES-DE\\es_log.txt") == "none (outside)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\covers\\Zinnia (USA).png") == "system:snes (media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\covers\\Sub\\Zinnia (USA).JPG") == "system:snes (media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\videos\\Zinnia (USA).mp4") == "system:snes (media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\manuals\\Zinnia (USA).pdf") == "system:snes (media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\covers") == "system:snes (media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes") == "system:snes (media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\covers\\Thumbs.db") == "none (not-media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\snes\\videos\\Zinnia (USA).flv.part") == "none (not-media)");
    CHECK(scopeOf(map, "X:\\Invented\\Media\\nes\\covers\\A.png") == "none (unknown-system)");
    CHECK(scopeOf(map, "X:\\Invented\\Media") == "everything (root)");
}

TEST_CASE("esde/change_scope: the roots watched and what their fallback polls", "[esde][change_scope]") {
    const std::vector<rv::core::WatchRoot> roots = rv::esde::watchRoots(invented());
    std::vector<std::string> names;
    for (const rv::core::WatchRoot& root : roots) {
        names.push_back(root.name + (root.subtree ? "" : " (own entries)"));
    }
    // The nested and the ROM root's systems are the ROM root's; psx lies outside it.
    CHECK(names == std::vector<std::string>{"settings (own entries)", "custom-systems (own entries)", "gamelists", "rom",
                                            "system:psx", "media"});
    CHECK(roots[0].pollTargets == std::vector<fs::path>{"X:\\Invented\\ES-DE\\settings\\es_settings.xml"});
    // The ROM root polls its system folders' level, never deeper.
    CHECK(roots[3].pollTargets ==
          std::vector<fs::path>{"Y:\\Roms", "Y:\\Roms\\gb", "Y:\\Roms\\snes", "Y:\\Roms\\snes\\msu"});
    CHECK(roots[4].pollTargets == std::vector<fs::path>{"Z:\\Discs\\psx"});
    CHECK(roots[2].pollTargets.size() == 5); // each system's gamelist file
    CHECK(roots[5].pollTargets.size() == 1 + 5 * 13); // the root, each system's folder and its kind folders

    // A map without roots (ES-DE's settings unread): the ES-DE home's folders still.
    ChangeScopeMap bare;
    bare.esdeHome = "X:\\Invented\\ES-DE";
    CHECK(rv::esde::watchRoots(bare).size() == 3);
}

TEST_CASE("esde/change_scope: the standard fixture's map", "[esde][change_scope]") {
    auto fixture = rv::test::Fixture::materialize("esde/standard");
    REQUIRE(fixture.has_value());
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture->path("ES-DE");
    profile.installFolder = fixture->path("Programs/ES-DE");
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    const auto systems = rv::esde::loadSystems(profile);
    REQUIRE(systems.has_value());
    const ChangeScopeMap map =
        rv::esde::changeScopeMap(profile, rv::esde::resolveRomRoot(*settings, profile),
                                 rv::esde::resolveMediaRoot(*settings, profile), *systems);
    REQUIRE(map.romRoot.has_value());
    REQUIRE(map.find("snes") != nullptr);
    CHECK(map.find("nes") == nullptr);
    CHECK(scopeOf(map, fixture->path("roms/snes/Fresh Find (USA).sfc")) == "system:snes (rom)");
    CHECK(scopeOf(map, fixture->path("roms/snes/Fresh Find (USA).srm")) == "none (extension)");
    CHECK(scopeOf(map, fixture->path("roms/psx/Solo Disc.cue")) == "system:psx (rom)");
    CHECK(scopeOf(map, fixture->path("ES-DE/gamelists/gb/gamelist.xml")) == "system:gb (gamelist)");
    CHECK(scopeOf(map, fixture->path("ES-DE/settings/es_settings.xml")) == "everything (settings)");
    // The index answers as the map does.
    const rv::esde::ChangeScopeIndex index(map);
    CHECK(describe(index.scope(fixture->path("roms/gb/Tiny Tennis (Europe).gb"))) == "system:gb (rom)");
}
