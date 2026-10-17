// I2 step 10: ES-DE's find rules evaluated as ES-DE 3.4.1 evaluates them (ADR-0008 §8), on FakeSystemProbe: App Paths
// in HKCU then HKLM, registry values with their suffix appended verbatim, the executable search (install folder,
// System32, the Windows folder, PATH; never anything else), static paths with '~', %ESPATH% and %ROMPATH%, wildcards
// matched as ES-DE's regular expression, the '|' replacement, the order of the rule types, corepath with %EMUPATH%, and
// the custom find rules first (on the custom-systems fixture).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <rv/core/system_probe.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/launch_translation.hpp>

#include "launch_test_machine.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::core::RegistryRoot;
using rv::esde::FindRule;
using rv::esde::FindRuleType;

namespace {

constexpr std::string_view kAppPaths = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";

std::string text(const fs::path& path) {
    return rv::core::pathToUtf8(path);
}

} // namespace

TEST_CASE("esde/find rules: App Paths in HKCU, then HKLM only when HKCU has no key", "[esde][launch]") {
    rv::test::LaunchMachine machine;
    machine.emulator("RETROARCH", {FindRule{FindRuleType::WinRegistryPath, {"retroarch.exe"}}});
    const std::string key = std::string(kAppPaths) + "retroarch.exe";

    SECTION("not registered") {
        const auto found = rv::esde::findEmulator(machine.context(), "RETROARCH");
        CHECK_FALSE(found.found);
        REQUIRE(found.attempts.size() == 1);
        CHECK(found.attempts[0] == "winregistrypath \"retroarch.exe\": not registered in App Paths");
    }
    SECTION("HKLM") {
        const fs::path exe = machine.program(machine.root() / "RetroArch" / "retroarch.exe");
        machine.probe.setRegistryString(RegistryRoot::LocalMachine, key, "", text(exe));
        const auto found = rv::esde::findEmulator(machine.context(), "RETROARCH");
        REQUIRE(found.found);
        CHECK(found.found->path == exe);
        CHECK(found.found->rule == "winregistrypath");
        CHECK(found.found->entry == "retroarch.exe");
        CHECK(found.found->detail == "HKLM");
    }
    SECTION("HKCU wins over HKLM") {
        const fs::path user = machine.program(machine.root() / "User RA" / "retroarch.exe");
        const fs::path system = machine.program(machine.root() / "RetroArch" / "retroarch.exe");
        machine.probe.setRegistryString(RegistryRoot::LocalMachine, key, "", text(system));
        machine.probe.setRegistryString(RegistryRoot::CurrentUser, key, "", text(user));
        const auto found = rv::esde::findEmulator(machine.context(), "RETROARCH");
        REQUIRE(found.found);
        CHECK(found.found->path == user);
        CHECK(found.found->detail == "HKCU");
    }
    SECTION("a HKCU key without a value hides HKLM's") {
        machine.program(machine.root() / "RetroArch" / "retroarch.exe");
        machine.probe.setRegistryString(RegistryRoot::LocalMachine, key, "",
                                        text(machine.root() / "RetroArch" / "retroarch.exe"));
        machine.probe.addRegistryKey(RegistryRoot::CurrentUser, key);
        const auto found = rv::esde::findEmulator(machine.context(), "RETROARCH");
        CHECK_FALSE(found.found);
        REQUIRE(found.attempts.size() == 1);
        CHECK(found.attempts[0].find("HKCU holds no path") != std::string::npos);
    }
    SECTION("a registered file that is gone, a quoted value: not found, and why") {
        machine.probe.setRegistryString(RegistryRoot::LocalMachine, key, "",
                                        text(machine.root() / "Gone" / "retroarch.exe"));
        auto found = rv::esde::findEmulator(machine.context(), "RETROARCH");
        CHECK_FALSE(found.found);
        CHECK(found.attempts.at(0).find("does not exist") != std::string::npos);
        // ES-DE keeps the quotes, and a quoted value names no file for it either.
        const fs::path exe = machine.program(machine.root() / "RetroArch" / "retroarch.exe");
        machine.probe.setRegistryString(RegistryRoot::LocalMachine, key, "", "\"" + text(exe) + "\"");
        found = rv::esde::findEmulator(machine.context(), "RETROARCH");
        CHECK_FALSE(found.found);
        CHECK(found.attempts.at(0).find("is not an absolute path") != std::string::npos);
    }
}

TEST_CASE("esde/find rules: registry values with the suffix appended verbatim", "[esde][launch]") {
    rv::test::LaunchMachine machine;
    const fs::path mgba = machine.program(machine.root() / "mGBA" / "mGBA.exe");
    const fs::path dosbox = machine.program(machine.root() / "DOSBox-X" / "dosbox-x.exe");
    machine.probe.setRegistryString(RegistryRoot::LocalMachine,
                                    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\mGBA_is1",
                                    "InstallLocation", text(machine.root() / "mGBA") + "\\");
    machine.probe.setRegistryString(RegistryRoot::CurrentUser, "SOFTWARE\\DOSBox-X", "Path",
                                    text(machine.root() / "DOSBox-X"));
    machine.emulator(
        "MGBA",
        {FindRule{FindRuleType::WinRegistryValue,
                  {"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\mGBA_is1\\InstallLocation|mGBA.exe"}}});
    machine.emulator("DOSBOX-X", {FindRule{FindRuleType::WinRegistryValue, {"SOFTWARE\\DOSBox-X\\Path|\\dosbox-x.exe"}}});
    machine.emulator("STEAM", {FindRule{FindRuleType::WinRegistryValue, {"SOFTWARE\\Valve\\Steam\\SteamExe"}}});

    // "InstallLocation|mGBA.exe" relies on the value's trailing backslash; "Path|\dosbox-x.exe" brings its own.
    const auto foundMgba = rv::esde::findEmulator(machine.context(), "MGBA");
    REQUIRE(foundMgba.found);
    CHECK(foundMgba.found->path == mgba);
    CHECK(foundMgba.found->rule == "winregistryvalue");
    CHECK(foundMgba.found->detail == "HKLM");
    const auto foundDosbox = rv::esde::findEmulator(machine.context(), "DOSBOX-X");
    REQUIRE(foundDosbox.found);
    CHECK(foundDosbox.found->path == dosbox);
    CHECK(foundDosbox.found->detail == "HKCU");
    const auto steam = rv::esde::findEmulator(machine.context(), "STEAM");
    CHECK_FALSE(steam.found);
    CHECK(steam.attempts.at(0) == "winregistryvalue \"SOFTWARE\\Valve\\Steam\\SteamExe\": no registry key "
                                  "SOFTWARE\\Valve\\Steam");
}

TEST_CASE("esde/find rules: the executable search looks in the install folder, System32, Windows and PATH only",
          "[esde][launch]") {
    rv::test::LaunchMachine machine;
    machine.emulator("EMU", {FindRule{FindRuleType::SystemPath, {"emu.exe"}}});
    machine.emulator("NOEXT", {FindRule{FindRuleType::SystemPath, {"noext"}}});
    const fs::path pathA = machine.root() / "Tools A";
    const fs::path pathB = machine.root() / "Tools B";
    machine.probe.setPathEntries({pathA, pathB});

    SECTION("PATH in its order, after the system folders") {
        machine.program(pathB / "emu.exe");
        auto found = rv::esde::findEmulator(machine.context(), "EMU");
        REQUIRE(found.found);
        CHECK(found.found->path == pathB / "emu.exe");
        CHECK(found.found->detail == text(pathB));
        machine.program(pathA / "emu.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "EMU").found->path == pathA / "emu.exe");
        machine.program(machine.root() / "Windows" / "emu.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "EMU").found->path == machine.root() / "Windows" / "emu.exe");
        machine.program(machine.root() / "Windows" / "System32" / "emu.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "EMU").found->path ==
              machine.root() / "Windows" / "System32" / "emu.exe");
        machine.program(machine.install() / "emu.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "EMU").found->path == machine.install() / "emu.exe");
    }
    SECTION("SearchPathW's default extension") {
        machine.program(pathA / "noext.exe");
        const auto found = rv::esde::findEmulator(machine.context(), "NOEXT");
        REQUIRE(found.found);
        CHECK(found.found->path == pathA / "noext.exe");
    }
    SECTION("nowhere else: not a ROM folder, not the current folder") {
        machine.program(machine.roms() / "PS1" / "emu.exe");
        machine.listed(fs::current_path() / "emu.exe"); // listed by the probe only: nothing is written there
        const auto found = rv::esde::findEmulator(machine.context(), "EMU");
        CHECK_FALSE(found.found);
        CHECK(found.attempts.at(0) == "systempath \"emu.exe\": not in the ES-DE install folder, System32, the Windows "
                                      "folder or the 2 folder(s) of PATH");
    }
    SECTION("an unknown install folder leaves the search undecided") {
        machine.profile.installFolder.reset();
        machine.program(pathA / "emu.exe");
        const auto found = rv::esde::findEmulator(machine.context(), "EMU");
        CHECK_FALSE(found.found);
        REQUIRE(found.undecided);
        CHECK(found.undecided->find("install folder") != std::string::npos);
    }
}

TEST_CASE("esde/find rules: static paths with '~', %ESPATH%, %ROMPATH% and wildcards", "[esde][launch]") {
    rv::test::LaunchMachine machine;

    SECTION("'~' is ES-DE's home folder, the ES-DE home's parent") {
        machine.emulator("HOME", {FindRule{FindRuleType::StaticPath, {"~\\Emulators\\home.exe"}}});
        machine.program(machine.home() / "ES-DE" / "Emulators" / "home.exe"); // read as the ES-DE home: not it
        CHECK_FALSE(rv::esde::findEmulator(machine.context(), "HOME").found);
        machine.program(machine.home() / "Emulators" / "home.exe");
        const auto found = rv::esde::findEmulator(machine.context(), "HOME");
        REQUIRE(found.found);
        CHECK(found.found->path == machine.home() / "Emulators" / "home.exe");
        CHECK(found.found->rule == "staticpath");
        CHECK(found.found->entry == "~\\Emulators\\home.exe");
    }
    SECTION("%ESPATH%, '..' and %ROMPATH%") {
        machine.emulator("ES", {FindRule{FindRuleType::StaticPath,
                                         {"%ESPATH%\\Emulators\\es.exe", "%ESPATH%\\..\\Emulators\\es.exe",
                                          "%ROMPATH%\\..\\Tools\\es.exe"}}});
        machine.program(machine.root() / "Tools" / "es.exe");
        auto found = rv::esde::findEmulator(machine.context(), "ES");
        REQUIRE(found.found);
        CHECK(found.found->path == machine.root() / "Tools" / "es.exe");
        CHECK(found.attempts.size() == 2);
        machine.program(machine.root() / "Emulators" / "es.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "ES").found->path == machine.root() / "Emulators" / "es.exe");
        machine.program(machine.install() / "Emulators" / "es.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "ES").found->path ==
              machine.install() / "Emulators" / "es.exe");
    }
    SECTION("%ESPATH% without an install folder, %ROMPATH% without a ROM root: undecided") {
        machine.emulator("ES", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\es.exe"}}});
        machine.emulator("RP", {FindRule{FindRuleType::StaticPath, {"%ROMPATH%\\rp.exe"}}});
        machine.profile.installFolder.reset();
        machine.romRoot.path.reset();
        CHECK(rv::esde::findEmulator(machine.context(), "ES").undecided);
        CHECK(rv::esde::findEmulator(machine.context(), "RP").undecided);
    }
    SECTION("a wildcard in the last element: ES-DE's regular expression, case-sensitive, the first in ordinal order") {
        const fs::path folder = machine.install() / "Emulators" / "PCSX2-Qt";
        machine.emulator("PCSX2", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\Emulators\\PCSX2-Qt\\pcsx2-qt*.exe"}}});
        machine.program(folder / "PCSX2-QT-upper.exe"); // another case: no match
        CHECK_FALSE(rv::esde::findEmulator(machine.context(), "PCSX2").found);
        machine.program(folder / "pcsx2-qtx64-avx2.exe");
        machine.program(folder / "pcsx2-qt.exe");
        auto found = rv::esde::findEmulator(machine.context(), "PCSX2");
        REQUIRE(found.found);
        CHECK(found.found->path == folder / "pcsx2-qt.exe"); // "pcsx2-qt.exe" < "pcsx2-qtx64-avx2.exe"
        // '.' stays a regular expression's wildcard character: "pcsx2-qt_exe" matches "pcsx2-qt*.exe" too.
        rv::test::LaunchMachine other;
        other.emulator("PCSX2", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\pcsx2-qt*.exe"}}});
        other.program(other.install() / "pcsx2-qt_exe");
        found = rv::esde::findEmulator(other.context(), "PCSX2");
        REQUIRE(found.found);
        CHECK(found.found->path == other.install() / "pcsx2-qt_exe");
    }
    SECTION("parentheses and brackets in a pattern are literal") {
        machine.emulator("PAREN", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\emu (x64)*.exe"}}});
        machine.program(machine.install() / "emu x64-1.exe");
        CHECK_FALSE(rv::esde::findEmulator(machine.context(), "PAREN").found);
        machine.program(machine.install() / "emu (x64) 2.exe");
        CHECK(rv::esde::findEmulator(machine.context(), "PAREN").found->path ==
              machine.install() / "emu (x64) 2.exe");
    }
    SECTION("a wildcard outside the last element matches nothing") {
        machine.emulator("DEEP", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\Emu*\\deep.exe"}}});
        machine.program(machine.install() / "Emulator" / "deep.exe");
        const auto found = rv::esde::findEmulator(machine.context(), "DEEP");
        CHECK_FALSE(found.found);
        CHECK(found.attempts.at(0).find("last element") != std::string::npos);
    }
    SECTION("a '|' replacement program: skipped while its path is missing, undecided once it exists") {
        machine.emulator("PIPE", {FindRule{FindRuleType::StaticPath,
                                           {"%ESPATH%\\Emulators\\pipe.exe|\"C:\\Tools\\launcher.exe\" --run",
                                            "%ESPATH%\\Emulators\\plain.exe"}}});
        machine.program(machine.install() / "Emulators" / "plain.exe");
        auto found = rv::esde::findEmulator(machine.context(), "PIPE");
        REQUIRE(found.found);
        CHECK(found.found->path == machine.install() / "Emulators" / "plain.exe");
        machine.program(machine.install() / "Emulators" / "pipe.exe");
        found = rv::esde::findEmulator(machine.context(), "PIPE");
        CHECK_FALSE(found.found);
        REQUIRE(found.undecided);
        CHECK(found.undecided->find("launcher.exe") != std::string::npos);
    }
}

TEST_CASE("esde/find rules: the rule types in ES-DE's order, whatever the document's", "[esde][launch]") {
    rv::test::LaunchMachine machine;
    const std::string key = std::string(kAppPaths) + "emu.exe";
    machine.emulator("EMU", {FindRule{FindRuleType::StaticPath, {"%ESPATH%\\emu.exe"}},
                             FindRule{FindRuleType::SystemPath, {"emu.exe"}},
                             FindRule{FindRuleType::WinRegistryValue, {"SOFTWARE\\Emu\\Dir|\\emu.exe"}},
                             FindRule{FindRuleType::WinRegistryPath, {"emu.exe"}}});
    const fs::path staticExe = machine.program(machine.install() / "emu.exe"); // also the systempath's first folder
    CHECK(rv::esde::findEmulator(machine.context(), "EMU").found->rule == "systempath");
    const fs::path valueExe = machine.program(machine.root() / "ByValue" / "emu.exe");
    machine.probe.setRegistryString(RegistryRoot::LocalMachine, "SOFTWARE\\Emu", "Dir", text(machine.root() / "ByValue"));
    CHECK(rv::esde::findEmulator(machine.context(), "EMU").found->path == valueExe);
    const fs::path appExe = machine.program(machine.root() / "ByAppPaths" / "emu.exe");
    machine.probe.setRegistryString(RegistryRoot::LocalMachine, key, "", text(appExe));
    const auto found = rv::esde::findEmulator(machine.context(), "EMU");
    CHECK(found.found->path == appExe);
    CHECK(found.found->rule == "winregistrypath");
    (void)staticExe;
    CHECK(rv::esde::findEmulator(machine.context(), "NOT-DEFINED").noRules);
}

TEST_CASE("esde/find rules: corepath with %EMUPATH%, the first core file that exists", "[esde][launch]") {
    rv::test::LaunchMachine machine;
    const fs::path emulatorFolder = machine.root() / "RetroArch";
    machine.core("RETROARCH", {"%EMUPATH%\\cores", "~\\Cores", "%ESPATH%\\Cores"});
    auto found = rv::esde::findCore(machine.context(), "RETROARCH", emulatorFolder, "gambatte_libretro.dll");
    CHECK_FALSE(found.found);
    REQUIRE(found.attempts.size() == 3);
    CHECK(found.attempts[0] == "corepath \"%EMUPATH%\\cores\": " + text(emulatorFolder / "cores" /
                                                                        "gambatte_libretro.dll") +
                                   " does not exist");
    machine.listed(machine.install() / "Cores" / "gambatte_libretro.dll");
    found = rv::esde::findCore(machine.context(), "RETROARCH", emulatorFolder, "gambatte_libretro.dll");
    REQUIRE(found.found);
    CHECK(found.text == text(machine.install()) + "\\Cores\\gambatte_libretro.dll");
    machine.listed(emulatorFolder / "cores" / "gambatte_libretro.dll");
    found = rv::esde::findCore(machine.context(), "RETROARCH", emulatorFolder, "gambatte_libretro.dll");
    REQUIRE(found.found);
    CHECK(found.found->path == emulatorFolder / "cores" / "gambatte_libretro.dll");
    CHECK(found.found->rule == "corepath");
    CHECK(found.found->entry == "%EMUPATH%\\cores");
    CHECK(found.text == text(emulatorFolder) + "\\cores\\gambatte_libretro.dll");
    CHECK(rv::esde::findCore(machine.context(), "NONE", emulatorFolder, "x.dll").noRules);
}

TEST_CASE("esde/find rules: the custom find rules come first (custom-systems fixture)", "[esde][launch]") {
    auto fixture = rv::test::Fixture::materialize("esde/custom-systems");
    REQUIRE(fixture.has_value());
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture->path("ES-DE");
    profile.installFolder = fixture->path("Programs/ES-DE");
    const rv::esde::FindRules rules = rv::esde::loadFindRules(profile);
    rv::test::FakeSystemProbe probe;
    rv::esde::ResolvedRoot romRoot;
    romRoot.path = fixture->path("roms");
    romRoot.esdeText = rv::core::pathToUtf8(fixture->path("roms")) + "\\";
    const rv::esde::TranslationContext context{profile, romRoot, &rules, &probe};

    // The bundled FAKE-EMULATOR's %ESPATH% path is not looked at: the custom definition replaced it.
    probe.addFile(fixture->path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe"));
    auto found = rv::esde::findEmulator(context, "FAKE-EMULATOR");
    CHECK_FALSE(found.found);
    REQUIRE(found.attempts.size() == 1);
    CHECK(found.attempts[0].find("~\\Custom Emulators\\fake-emulator.exe") != std::string::npos);
    probe.addFile(fixture->root() / "Custom Emulators" / "fake-emulator.exe");
    found = rv::esde::findEmulator(context, "FAKE-EMULATOR");
    REQUIRE(found.found);
    CHECK(found.found->path == fixture->root() / "Custom Emulators" / "fake-emulator.exe");

    // CUSTOM-EMULATOR: winregistrypath (written last) is tried first, then the value, the search, the static paths.
    found = rv::esde::findEmulator(context, "CUSTOM-EMULATOR");
    CHECK_FALSE(found.found);
    REQUIRE(found.attempts.size() == 5);
    CHECK(found.attempts[0].starts_with("winregistrypath \"custom-emulator.exe\""));
    CHECK(found.attempts[1].starts_with("winregistryvalue \"SOFTWARE\\Custom Emulator\\InstallDir|"));
    CHECK(found.attempts[2].starts_with("systempath \"custom-emulator.exe\""));
    CHECK(found.attempts[3].starts_with("staticpath \"%ESPATH%\\Emulators\\Custom\\custom-emulator-*.exe\""));
    CHECK(found.attempts[4].starts_with("staticpath \"%ROMPATH%\\..\\Emulators\\custom-emulator.exe|"));
    probe.addFile(fixture->path("Programs/ES-DE/Emulators/Custom/custom-emulator-2.0.exe"));
    probe.addFile(fixture->path("Programs/ES-DE/Emulators/Custom/custom-emulator-10.0.exe"));
    found = rv::esde::findEmulator(context, "CUSTOM-EMULATOR");
    REQUIRE(found.found);
    CHECK(found.found->path == fixture->path("Programs/ES-DE/Emulators/Custom/custom-emulator-10.0.exe"));

    // The custom RETROARCH core replaces the bundled %EMUPATH%\cores.
    const auto core = rv::esde::findCore(context, "RETROARCH", fixture->root() / "RetroArch", "fake_nes_libretro.dll");
    REQUIRE(core.attempts.size() == 1);
    CHECK(core.attempts[0].starts_with("corepath \"~\\Custom Cores\""));
}
