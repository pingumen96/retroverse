// The rules ES-DE applies on top of the system probe (App Paths precedence, the uninstall entry, wildcards), on a
// machine described by tests/support/fake_system_probe.hpp.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <rv/core/system_probe.hpp>

#include "support/fake_system_probe.hpp"

namespace fs = std::filesystem;
using rv::core::appPath;
using rv::core::findUninstallEntry;
using rv::core::matchesWildcard;
using rv::core::RegistryRoot;
using rv::test::FakeSystemProbe;

namespace {

constexpr std::string_view kAppPaths = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\retroarch.exe";
constexpr std::string_view kUninstall = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
constexpr std::string_view kUninstall32 = "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

} // namespace

TEST_CASE("core/system_probe: App Paths, HKCU first, HKLM only without a HKCU key", "[core][system_probe]") {
    SECTION("HKLM alone") {
        FakeSystemProbe probe;
        probe.setRegistryString(RegistryRoot::LocalMachine, kAppPaths, "", "C:\\Apps\\RetroArch\\retroarch.exe");
        CHECK(appPath(probe, "retroarch.exe") == fs::path(L"C:\\Apps\\RetroArch\\retroarch.exe"));
        CHECK(appPath(probe, "RetroArch.EXE") == fs::path(L"C:\\Apps\\RetroArch\\retroarch.exe"));
        CHECK_FALSE(appPath(probe, "duckstation.exe").has_value());
    }
    SECTION("HKCU wins over HKLM") {
        FakeSystemProbe probe;
        probe.setRegistryString(RegistryRoot::LocalMachine, kAppPaths, "", "C:\\machine\\retroarch.exe");
        probe.setRegistryString(RegistryRoot::CurrentUser, kAppPaths, "", "\"C:\\user\\retroarch.exe\"");
        CHECK(appPath(probe, "retroarch.exe") == fs::path(L"C:\\user\\retroarch.exe")); // quotes removed
    }
    SECTION("a HKCU key without a default value hides HKLM's") {
        FakeSystemProbe probe;
        probe.setRegistryString(RegistryRoot::LocalMachine, kAppPaths, "", "C:\\machine\\retroarch.exe");
        probe.setRegistryString(RegistryRoot::CurrentUser, kAppPaths, "Path", "C:\\user");
        CHECK_FALSE(appPath(probe, "retroarch.exe").has_value());
    }
    SECTION("names that are not file names") {
        FakeSystemProbe probe;
        probe.setRegistryString(RegistryRoot::LocalMachine, kAppPaths, "", "C:\\machine\\retroarch.exe");
        CHECK_FALSE(appPath(probe, "").has_value());
        CHECK_FALSE(appPath(probe, "..\\App Paths\\retroarch.exe").has_value());
        CHECK_FALSE(appPath(probe, "x/retroarch.exe").has_value());
    }
    SECTION("an empty value") {
        FakeSystemProbe probe;
        probe.setRegistryString(RegistryRoot::LocalMachine, kAppPaths, "", "  ");
        CHECK_FALSE(appPath(probe, "retroarch.exe").has_value());
    }
}

TEST_CASE("core/system_probe: the uninstall entry of ES-DE", "[core][system_probe]") {
    SECTION("as the ES-DE installer writes it: the version after the name, DisplayIcon only") {
        FakeSystemProbe probe;
        const std::string key = std::string(kUninstall32) + "\\ES-DE";
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayName", "ES-DE 3.4.1");
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayVersion", "3.4.1");
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayIcon", "C:\\Apps\\ES-DE\\ES-DE.exe");
        probe.setRegistryString(RegistryRoot::LocalMachine, std::string(kUninstall32) + "\\Other", "DisplayName",
                                "ES-DE Theme Pack 1.0");
        const auto entry = findUninstallEntry(probe, "ES-DE");
        REQUIRE(entry.has_value());
        CHECK(entry->root == RegistryRoot::LocalMachine);
        CHECK(entry->keyPath == key);
        CHECK(entry->displayName == "ES-DE 3.4.1");
        CHECK(entry->displayVersion == "3.4.1");
        CHECK_FALSE(entry->installLocation.has_value());
        CHECK(entry->displayIcon == fs::path(L"C:\\Apps\\ES-DE\\ES-DE.exe"));
        CHECK(entry->installFolder() == fs::path(L"C:\\Apps\\ES-DE"));
        CHECK_FALSE(findUninstallEntry(probe, "ES").has_value());
        CHECK_FALSE(findUninstallEntry(probe, "").has_value());
    }
    SECTION("InstallLocation wins; icon indexes and quotes are removed") {
        FakeSystemProbe probe;
        const std::string key = std::string(kUninstall) + "\\{GUID}";
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayName", "es-de");
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "InstallLocation", "\"D:\\Apps\\ES-DE\\\"");
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayIcon", "\"D:\\Apps\\ES-DE\\ES-DE.exe\",0");
        const auto entry = findUninstallEntry(probe, "ES-DE");
        REQUIRE(entry.has_value());
        CHECK(entry->installLocation == fs::path(L"D:\\Apps\\ES-DE\\"));
        CHECK(entry->displayIcon == fs::path(L"D:\\Apps\\ES-DE\\ES-DE.exe"));
        CHECK(entry->installFolder() == fs::path(L"D:\\Apps\\ES-DE\\"));
    }
    SECTION("an icon index without quotes") {
        FakeSystemProbe probe;
        const std::string key = std::string(kUninstall) + "\\ES-DE";
        probe.setRegistryString(RegistryRoot::CurrentUser, key, "DisplayName", "ES-DE");
        probe.setRegistryString(RegistryRoot::CurrentUser, key, "DisplayIcon", "C:\\Users\\X\\ES-DE\\ES-DE.exe,-101");
        const auto entry = findUninstallEntry(probe, "ES-DE");
        REQUIRE(entry.has_value());
        CHECK(entry->displayIcon == fs::path(L"C:\\Users\\X\\ES-DE\\ES-DE.exe"));
    }
    SECTION("HKCU before HKLM, the native view before WOW6432Node") {
        FakeSystemProbe probe;
        probe.setRegistryString(RegistryRoot::LocalMachine, std::string(kUninstall32) + "\\ES-DE", "DisplayName",
                                "ES-DE");
        probe.setRegistryString(RegistryRoot::LocalMachine, std::string(kUninstall) + "\\ES-DE", "DisplayName",
                                "ES-DE");
        CHECK(findUninstallEntry(probe, "ES-DE")->keyPath == std::string(kUninstall) + "\\ES-DE");
        probe.setRegistryString(RegistryRoot::CurrentUser, std::string(kUninstall32) + "\\ES-DE", "DisplayName",
                                "ES-DE");
        CHECK(findUninstallEntry(probe, "ES-DE")->root == RegistryRoot::CurrentUser);
    }
    SECTION("a version that does not match the one after the name") {
        FakeSystemProbe probe;
        const std::string key = std::string(kUninstall) + "\\ES-DE";
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayName", "ES-DE 3.4.1");
        probe.setRegistryString(RegistryRoot::LocalMachine, key, "DisplayVersion", "3.5.0");
        CHECK_FALSE(findUninstallEntry(probe, "ES-DE").has_value());
    }
}

TEST_CASE("core/system_probe: wildcards as the file system matches names", "[core][system_probe]") {
    CHECK(matchesWildcard("retroarch.exe", "retroarch.exe"));
    CHECK(matchesWildcard("RetroArch.EXE", "retroarch.exe"));
    CHECK(matchesWildcard("retroarch.exe", "*.exe"));
    CHECK(matchesWildcard("retroarch.exe", "retro*"));
    CHECK(matchesWildcard("retroarch.exe", "*"));
    CHECK(matchesWildcard("retroarch.exe", "r?troarch.e*"));
    CHECK(matchesWildcard("pcsx2-qt.exe", "pcsx2*.exe"));
    CHECK(matchesWildcard("pcsx2.exe", "pcsx2*.exe")); // '*' also stands for nothing
    CHECK(matchesWildcard("Pok\xC3\xA9mon.exe", "POK\xC3\x89*"));
    CHECK(matchesWildcard("\xF0\x9F\x8E\xAE.exe", "?.exe")); // '?' is one character, not one UTF-16 unit
    CHECK_FALSE(matchesWildcard("retroarch.exe", "*.com"));
    CHECK_FALSE(matchesWildcard("x.html", "*.htm"));          // no 8.3 short-name matches
    CHECK_FALSE(matchesWildcard("retroarch.exe", "retroarch"));
    CHECK_FALSE(matchesWildcard("ab", "a?b"));
    CHECK_FALSE(matchesWildcard("bad\xC3", "*"));
}

TEST_CASE("core/system_probe: the fake probe's files, folders and registry", "[core][system_probe]") {
    FakeSystemProbe probe;
    probe.addFile(fs::path(L"E:\\Emu\\Dolphin-x64\\Dolphin.exe"))
        .addFile(fs::path(L"E:\\Emu\\Dolphin-x64\\dolphintool.exe"))
        .addFile(fs::path(L"E:\\Emu\\readme.txt"))
        .setHomeFolder(fs::path(L"C:\\Users\\X"))
        .setPathEntries({fs::path(L"C:\\Tools")});
    CHECK(probe.fileExists(fs::path(L"e:\\emu\\dolphin-x64\\DOLPHIN.EXE")));
    CHECK_FALSE(probe.fileExists(fs::path(L"E:\\Emu\\Dolphin-x64")));
    CHECK(probe.filesMatching(fs::path(L"E:\\Emu\\Dolphin-x64"), "*.exe") ==
          std::vector<fs::path>{fs::path(L"E:\\Emu\\Dolphin-x64\\Dolphin.exe"),
                                fs::path(L"E:\\Emu\\Dolphin-x64\\dolphintool.exe")});
    CHECK(probe.filesMatching(fs::path(L"E:\\Emu"), "*.exe").empty());
    CHECK(probe.homeFolder() == fs::path(L"C:\\Users\\X"));
    CHECK(probe.pathEntries() == std::vector<fs::path>{fs::path(L"C:\\Tools")});

    probe.setRegistryString(RegistryRoot::LocalMachine, "SOFTWARE\\Vendor\\Emu", "InstallDir", "E:\\Emu");
    CHECK(probe.registryKeyExists(RegistryRoot::LocalMachine, "software\\vendor"));
    CHECK_FALSE(probe.registryKeyExists(RegistryRoot::CurrentUser, "SOFTWARE\\Vendor"));
    CHECK(probe.registryString(RegistryRoot::LocalMachine, "Software\\Vendor\\EMU", "installdir") == "E:\\Emu");
    CHECK_FALSE(probe.registryString(RegistryRoot::LocalMachine, "SOFTWARE\\Vendor\\Emu", "").has_value());
    CHECK(probe.registrySubKeys(RegistryRoot::LocalMachine, "SOFTWARE") == std::vector<std::string>{"Vendor"});
}
