// The MAME lists (I2 step 7): read as ES-DE 3.4.1's MameNames reads them, looked up with case, and the names ES-DE
// gives arcade games (removeParenthesis, MAMENameStripExtraInfo); the platforms that make a system an arcade one; ES-DE's
// extension and stem of a name.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <rv/esde/mame_sets.hpp>
#include <rv/esde/rom_scanner.hpp>

#include "support/file_system.hpp"
#include "support/temp_dir.hpp"

TEST_CASE("esde/mame_sets: removeParenthesis as ES-DE's", "[esde][mame]") {
    using rv::esde::esdeRemoveParenthesis;
    CHECK(esdeRemoveParenthesis("Zap Zone (World, set 1)") == "Zap Zone");
    CHECK(esdeRemoveParenthesis("Zap Zone (Japan) [bootleg]") == "Zap Zone");
    CHECK(esdeRemoveParenthesis("A (b (c) d) e") == "A  e");
    CHECK(esdeRemoveParenthesis("  spaced\t") == "spaced");
    CHECK(esdeRemoveParenthesis("(all)") == "");
    CHECK(esdeRemoveParenthesis("no brackets") == "no brackets");
    // A closing bracket with no opening one before it ends the removals of its kind.
    CHECK(esdeRemoveParenthesis("a) (b)") == "a) (b)");
    CHECK(esdeRemoveParenthesis("x ( y") == "x ( y");
    // Mixed kinds: the square pair is removed with the round bracket inside it.
    CHECK(esdeRemoveParenthesis("[a)b] c") == "c");
}

TEST_CASE("esde/mame_sets: lookups compare exactly; the last real name of a set counts", "[esde][mame]") {
    const rv::esde::MameSets sets({{"zz", "First"}, {"aa", "Alpha (set 1)"}, {"zz", "Second"}, {"empty", ""}},
                                  {"biosb", "biosa"}, {"devc"});
    CHECK(sets.nameCount() == 3);
    CHECK(sets.realName("zz") == "Second");
    CHECK(sets.realName("aa") == "Alpha (set 1)");
    CHECK(sets.realName("empty") == "empty");
    CHECK(sets.realName("unknown") == "unknown");
    CHECK(sets.realName("ZZ") == "ZZ");
    CHECK(sets.isBios("biosa"));
    CHECK(sets.isBios("biosb"));
    CHECK_FALSE(sets.isBios("BIOSA"));
    CHECK(sets.isDevice("devc"));
    CHECK(sets.isBiosOrDevice("devc"));
    CHECK_FALSE(sets.isBiosOrDevice("aa"));
    CHECK(rv::esde::mameName(sets, "aa", true) == "Alpha");
    CHECK(rv::esde::mameName(sets, "aa", false) == "Alpha (set 1)");
    CHECK(rv::esde::mameName(sets, "aa (alt)", true) == "Alpha");
    CHECK(rv::esde::mameName(sets, "(x)", true).empty());
}

TEST_CASE("esde/mame_sets: the lists read in ES-DE's order, a missing one ending the reading", "[esde][mame]") {
    const rv::test::TempDir dir("rv-mame-lists");
    REQUIRE(rv::test::writeFileAnyLength(dir.file("names.xml"),
                                         "<!-- c -->\n<game><mamename>a&amp;b</mamename><realname> R </realname></game>"
                                         "\n<game><mamename>x</mamename></game>\n"));
    REQUIRE(rv::test::writeFileAnyLength(dir.file("bioses.xml"), "<bios>b1</bios>\n<bios>b2</bios>\n"));
    REQUIRE(rv::test::writeFileAnyLength(dir.file("devices.xml"), "<device>d1</device>\n"));
    REQUIRE(rv::test::writeFileAnyLength(dir.file("broken.xml"), "<bios>b1</bi"));

    const auto all = rv::esde::readMameListFiles(dir.file("names.xml"), dir.file("bioses.xml"), dir.file("devices.xml"));
    CHECK(all.issues.empty());
    CHECK(all.files.size() == 3);
    CHECK(all.sets.realName("a&b") == " R "); // never trimmed
    CHECK(all.sets.realName("x") == "x");
    CHECK(all.sets.isBios("b2"));
    CHECK(all.sets.isDevice("d1"));

    const auto noNames = rv::esde::readMameListFiles(std::nullopt, dir.file("bioses.xml"), dir.file("devices.xml"));
    REQUIRE(noNames.issues.size() == 1);
    CHECK(noNames.issues[0].code == "mame-list-missing");
    CHECK_FALSE(noNames.sets.isBios("b1"));
    CHECK_FALSE(noNames.sets.isDevice("d1"));

    const auto brokenBioses =
        rv::esde::readMameListFiles(dir.file("names.xml"), dir.file("broken.xml"), dir.file("devices.xml"));
    REQUIRE(brokenBioses.issues.size() == 1);
    CHECK(brokenBioses.issues[0].code == "mame-list-unreadable");
    CHECK(brokenBioses.sets.nameCount() == 2);
    CHECK_FALSE(brokenBioses.sets.isDevice("d1"));
}

TEST_CASE("esde/mame_sets: the arcade platforms", "[esde][mame]") {
    using rv::esde::isArcadePlatform;
    CHECK(isArcadePlatform(std::vector<std::string>{"arcade"}));
    CHECK(isArcadePlatform(std::vector<std::string>{"neogeo"}));
    CHECK(isArcadePlatform(std::vector<std::string>{"daphne", "arcade"}));
    CHECK_FALSE(isArcadePlatform(std::vector<std::string>{"snes"}));
    CHECK_FALSE(isArcadePlatform(std::vector<std::string>{"arcade", "ignore"}));
    CHECK_FALSE(isArcadePlatform(std::vector<std::string>{"ignore", "arcade"}));
    CHECK_FALSE(isArcadePlatform(std::vector<std::string>{}));
}

TEST_CASE("esde/rom_scanner: ES-DE's extension and stem of a name", "[esde][rom_scan]") {
    using rv::esde::esdeExtension;
    using rv::esde::esdeStem;
    CHECK(esdeExtension("Game.sfc") == ".sfc");
    CHECK(esdeExtension("Game.dec.iso") == ".iso");
    CHECK(esdeExtension("Game") == ".");
    CHECK(esdeExtension("Game.") == ".");
    CHECK(esdeExtension(".gb") == ".gb");
    CHECK(esdeExtension("Sports Vol. 2") == ". 2");
    CHECK(esdeStem("Game.dec.iso", false) == "Game.dec");
    CHECK(esdeStem(".gb", false) == "");
    CHECK(esdeStem("Game", false) == "Game");
    CHECK(esdeStem("Folder.ps3", true) == "Folder.ps3");

    rv::esde::ScanRules rules;
    CHECK(rv::esde::esdeDefaultName("Sub/Disk .atr", rv::esde::EsdeNameKind::File, rules) == "Disk");
    CHECK(rv::esde::esdeDefaultName("Space (Europe).ps3", rv::esde::EsdeNameKind::GameDirectory, rules) ==
          "Space (Europe)");
    CHECK(rv::esde::esdeDefaultName("Sports Vol. 2", rv::esde::EsdeNameKind::Folder, rules) == "Sports Vol. 2");
    std::string own;
    CHECK(rv::esde::esdeDefaultName(".gb", rv::esde::EsdeNameKind::File, rules, &own) == ".gb");
    CHECK(own.empty()); // ES-DE skips the file "as it has no filename"
    const rv::esde::MameSets sets({{"zap", "Zap (World)"}}, {}, {});
    rules.arcade = true;
    rules.mame = &sets;
    CHECK(rv::esde::esdeDefaultName("zap.zip", rv::esde::EsdeNameKind::File, rules) == "Zap");
    CHECK(rv::esde::esdeDefaultName("zap (alt).zip", rv::esde::EsdeNameKind::File, rules) == "Zap");
    CHECK(rv::esde::esdeDefaultName("Folder (x)", rv::esde::EsdeNameKind::Folder, rules) == "Folder (x)");
    CHECK(rv::esde::esdeDefaultName("(x).zip", rv::esde::EsdeNameKind::File, rules, &own) == "(x)");
    CHECK(own.empty());
    rules.mameNameStripExtraInfo = false;
    CHECK(rv::esde::esdeDefaultName("zap.zip", rv::esde::EsdeNameKind::File, rules) == "Zap (World)");
}
