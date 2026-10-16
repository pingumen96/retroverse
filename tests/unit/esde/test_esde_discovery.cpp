// Discovery of the ES-DE home and install folder (I2 step 2) on fixtures, the machine described by a fake probe: the
// uninstall entry and its version, explicit folders and ES-DE's --home, portable.txt read as ES-DE 3.4.1 reads it, the
// default home; where bundled resources come from; whether a folder is there; library.json read and written.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/settings.hpp>

#include "support/fake_system_probe.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::core::RegistryRoot;
using rv::esde::DiscoveryOptions;
using rv::esde::EsdeDiscovery;
using rv::esde::HomeSource;
using rv::esde::InstallSource;

namespace {

constexpr std::string_view kUninstallKey =
    "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ES-DE";

rv::test::Fixture materialize(std::string_view name) {
    auto fixture = rv::test::Fixture::materialize(name);
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    return std::move(*fixture);
}

/// A machine where ES-DE's installer registered `install` (as ES-DE 3.4.1's installer does: DisplayName with the
/// version, DisplayIcon quoted with an icon index) and whose user's home folder is `home`.
rv::test::FakeSystemProbe machine(const std::optional<fs::path>& install, const std::optional<fs::path>& home) {
    rv::test::FakeSystemProbe probe;
    if (install) {
        probe.setRegistryString(RegistryRoot::LocalMachine, kUninstallKey, "DisplayName", "ES-DE 3.4.1");
        probe.setRegistryString(RegistryRoot::LocalMachine, kUninstallKey, "DisplayVersion", "3.4.1");
        probe.setRegistryString(RegistryRoot::LocalMachine, kUninstallKey, "DisplayIcon",
                                "\"" + rv::core::pathToUtf8(*install / "ES-DE.exe") + "\",0");
    }
    probe.setHomeFolder(home);
    return probe;
}

EsdeDiscovery discover(const DiscoveryOptions& options, const rv::core::ISystemProbe& probe) {
    auto discovery = rv::esde::discoverEsde(options, probe);
    INFO((discovery ? "discovered" : discovery.error().toString()));
    REQUIRE(discovery.has_value());
    return std::move(*discovery);
}

std::vector<std::string> codesOf(const std::vector<rv::catalog::Issue>& issues) {
    std::vector<std::string> codes;
    for (const auto& issue : issues) {
        codes.push_back(issue.code);
    }
    return codes;
}

void writePortable(const rv::test::Fixture& fixture, std::string_view content) {
    REQUIRE(rv::core::writeTextFileAtomic(fixture.path("ES-DE/portable.txt"), content).has_value());
}

/// A drive letter that names no drive on this machine.
std::optional<char> unusedDrive() {
    const DWORD drives = GetLogicalDrives();
    for (char letter = 'Z'; letter >= 'D'; --letter) {
        if ((drives & (1u << (letter - 'A'))) == 0) {
            return letter;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("esde/discovery: the install folder through its uninstall entry, the version, the default home",
          "[esde][discovery]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const fs::path install = fixture.path("Programs/ES-DE");
    const auto probe = machine(install, fixture.root());

    const EsdeDiscovery found = discover({}, probe);
    REQUIRE(found.installFolder.has_value());
    CHECK(*found.installFolder == install);
    CHECK(found.installSource == InstallSource::UninstallEntry);
    CHECK(found.uninstallKey == "HKLM\\" + std::string(kUninstallKey));
    CHECK(found.version == "3.4.1");
    CHECK(found.executableFound);
    CHECK_FALSE(found.portableFile.has_value());
    REQUIRE(found.esdeHome.has_value());
    CHECK(*found.esdeHome == fixture.path("ES-DE"));
    CHECK(found.homeSource == HomeSource::Default);
    CHECK(found.esdeHomeExists);
    CHECK(found.homeFolder() == fixture.root());
    CHECK(found.issues.empty());

    const auto profile = rv::esde::profileOf(found, rv::catalog::LibraryId());
    REQUIRE(profile.has_value());
    CHECK(profile->id.str() == "esde");
    CHECK(profile->esdeHome == fixture.path("ES-DE"));
    CHECK(profile->installFolder == install);
    CHECK_FALSE(profile->romRoot.has_value());
    CHECK_FALSE(profile->mediaRoot.has_value());

    // InstallLocation is preferred to DisplayIcon; the name may be "ES-DE" alone.
    rv::test::FakeSystemProbe located = machine(std::nullopt, fixture.root());
    constexpr std::string_view kUserKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ESDE";
    located.setRegistryString(RegistryRoot::CurrentUser, kUserKey, "DisplayName", "ES-DE");
    located.setRegistryString(RegistryRoot::CurrentUser, kUserKey, "InstallLocation",
                              rv::core::pathToUtf8(install) + "\\");
    const EsdeDiscovery byLocation = discover({}, located);
    CHECK(byLocation.installFolder == install);
    CHECK(byLocation.version.empty()); // no DisplayVersion
    CHECK(byLocation.uninstallKey == "HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ESDE");
}

TEST_CASE("esde/discovery: explicit folders win; --home names the folder that holds the ES-DE home",
          "[esde][discovery]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const fs::path install = fixture.path("Programs/ES-DE");
    const auto probe = machine(install, fixture.path("Elsewhere"));

    DiscoveryOptions options;
    options.homeFolder = fixture.root();
    options.installFolder = install;
    const EsdeDiscovery given = discover(options, probe);
    CHECK(given.esdeHome == fixture.path("ES-DE"));
    CHECK(given.homeSource == HomeSource::Explicit);
    CHECK(given.installSource == InstallSource::Explicit);
    CHECK(given.version == "3.4.1"); // the registered install folder is this one
    CHECK(given.issues.empty());

    DiscoveryOptions home;
    home.esdeHome = fixture.root() / "." / "ES-DE" / "";
    const EsdeDiscovery explicitHome = discover(home, probe);
    CHECK(explicitHome.esdeHome == fixture.path("ES-DE")); // tidied: no "." element, no trailing separator
    CHECK(explicitHome.homeFolder() == fixture.root());

    // Another install folder than the registered one: no version.
    DiscoveryOptions other;
    other.installFolder = fixture.path("Emulators");
    const EsdeDiscovery elsewhere = discover(other, probe);
    CHECK(elsewhere.installFolder == fixture.path("Emulators"));
    CHECK(elsewhere.version.empty());
    CHECK_FALSE(elsewhere.executableFound);
    CHECK(codesOf(elsewhere.issues) == std::vector<std::string>{"install-incomplete", "home-missing"});

    // --home naming the ES-DE home itself finds no ES-DE home inside it.
    DiscoveryOptions mistaken;
    mistaken.homeFolder = fixture.path("ES-DE");
    const EsdeDiscovery wrong = discover(mistaken, probe);
    CHECK(wrong.esdeHome == fixture.path("ES-DE/ES-DE"));
    CHECK_FALSE(wrong.esdeHomeExists);
    CHECK(codesOf(wrong.issues) == std::vector<std::string>{"home-missing"});
    CHECK(wrong.issues[0].reason.find("looks like the ES-DE home itself") != std::string::npos);
}

TEST_CASE("esde/discovery: a portable installation, its portable.txt read as ES-DE reads it", "[esde][discovery]") {
    const rv::test::Fixture fixture = materialize("esde/portable");
    const fs::path install = fixture.path("ES-DE");
    const auto probe = machine(install, fixture.path("Users/Someone"));

    SECTION("an empty portable.txt: ES-DE's home folder is the install folder") {
        const EsdeDiscovery found = discover({}, probe);
        CHECK(found.installFolder == install);
        CHECK(found.portableFile == install / "portable.txt");
        CHECK(found.homeSource == HomeSource::Portable);
        CHECK(found.esdeHome == install / "ES-DE");
        CHECK(found.homeFolder() == install);
        CHECK(found.esdeHomeExists);
        CHECK(found.issues.empty());
        // Empty settings: the ROMs and the media under the portable folders.
        const auto profile = rv::esde::profileOf(found, rv::catalog::LibraryId());
        REQUIRE(profile.has_value());
        const auto settings = rv::esde::readSettings(*found.esdeHome);
        REQUIRE(settings.has_value());
        const auto romRoot = rv::esde::resolveRomRoot(*settings, *profile);
        CHECK(romRoot.path == install / "ROMs");
        CHECK(romRoot.source == rv::esde::RootSource::Default);
        const auto mediaRoot = rv::esde::resolveMediaRoot(*settings, *profile);
        CHECK(mediaRoot.path == install / "ES-DE" / "downloaded_media");
        CHECK(mediaRoot.source == rv::esde::RootSource::Default);
    }
    SECTION("a relative folder on the first line (CRLF, more lines after it)") {
        writePortable(fixture, "..\\PortableHome\r\nnot read\r\n");
        const EsdeDiscovery found = discover({}, probe);
        CHECK(found.homeSource == HomeSource::Portable);
        CHECK(found.esdeHome == fixture.path("PortableHome/ES-DE"));
        CHECK(found.homeFolder() == fixture.path("PortableHome"));
        CHECK(found.issues.empty());
        // %ESPATH% is the install folder, '~' the portable home folder.
        const auto profile = rv::esde::profileOf(found, rv::catalog::LibraryId());
        REQUIRE(profile.has_value());
        const auto settings = rv::esde::readSettings(*found.esdeHome);
        REQUIRE(settings.has_value());
        CHECK(rv::esde::resolveRomRoot(*settings, *profile).path == install / "ROMs");
        CHECK(rv::esde::resolveMediaRoot(*settings, *profile).path == fixture.path("PortableHome/Media"));

        writePortable(fixture, "../PortableHome\x1A" "ignored after Ctrl+Z");
        CHECK(discover({}, probe).esdeHome == fixture.path("PortableHome/ES-DE"));
    }
    SECTION("a home folder ES-DE rejects: the default home") {
        for (const std::string_view content : {std::string_view("Missing Home"), std::string_view("ES-DE.exe"),
                                               std::string_view("\xEF\xBB\xBF"), std::string_view("C:\\Absolute")}) {
            INFO("portable.txt: " << content);
            writePortable(fixture, content);
            const EsdeDiscovery found = discover({}, probe);
            CHECK(found.homeSource == HomeSource::Default);
            CHECK(found.esdeHome == fixture.path("Users/Someone/ES-DE"));
            CHECK(found.portableFile == install / "portable.txt");
            CHECK(codesOf(found.issues) == std::vector<std::string>{"portable-home-invalid"});
        }
    }
    SECTION("an explicit home wins over portable.txt") {
        DiscoveryOptions options;
        options.homeFolder = fixture.path("PortableHome");
        const EsdeDiscovery found = discover(options, probe);
        CHECK(found.homeSource == HomeSource::Explicit);
        CHECK(found.esdeHome == fixture.path("PortableHome/ES-DE"));
        CHECK(found.portableFile == install / "portable.txt");
    }
    SECTION("no portable.txt: the default home") {
        REQUIRE(DeleteFileW(install.wstring().append(L"\\portable.txt").c_str()) != 0);
        const EsdeDiscovery found = discover({}, probe);
        CHECK_FALSE(found.portableFile.has_value());
        CHECK(found.homeSource == HomeSource::Default);
        CHECK(found.esdeHome == fixture.path("Users/Someone/ES-DE"));
        CHECK(found.esdeHomeExists);
    }
}

TEST_CASE("esde/discovery: nothing to find, invalid arguments", "[esde][discovery]") {
    const rv::test::FakeSystemProbe empty;
    const EsdeDiscovery nothing = discover({}, empty);
    CHECK_FALSE(nothing.installFolder.has_value());
    CHECK(nothing.installSource == InstallSource::None);
    CHECK_FALSE(nothing.esdeHome.has_value());
    CHECK(nothing.homeSource == HomeSource::None);
    CHECK_FALSE(nothing.homeFolder().has_value());
    CHECK(codesOf(nothing.issues) == std::vector<std::string>{"install-not-found", "home-unknown"});
    const auto profile = rv::esde::profileOf(nothing, rv::catalog::LibraryId());
    REQUIRE_FALSE(profile.has_value());
    CHECK(profile.error().code == rv::ErrorCode::NotFound);

    DiscoveryOptions relative;
    relative.installFolder = fs::path(L"Programs\\ES-DE");
    const auto relativeFound = rv::esde::discoverEsde(relative, empty);
    REQUIRE_FALSE(relativeFound.has_value());
    CHECK(relativeFound.error().code == rv::ErrorCode::InvalidArgument);
    DiscoveryOptions both;
    both.esdeHome = fs::path(L"C:\\Users\\x\\ES-DE");
    both.homeFolder = fs::path(L"C:\\Users\\x");
    const auto bothFound = rv::esde::discoverEsde(both, empty);
    REQUIRE_FALSE(bothFound.has_value());
    CHECK(bothFound.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("esde/discovery: bundled resources come from the ES-DE home's resources first, then the install folder's",
          "[esde][discovery]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture.path("ES-DE");
    profile.installFolder = fixture.path("Programs/ES-DE");
    constexpr std::string_view kSystems = "systems/windows/es_systems.xml";

    CHECK(rv::esde::bundledResource(profile, kSystems) ==
          fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml"));
    REQUIRE(rv::core::writeTextFileAtomic(fixture.path("ES-DE/resources/systems/windows/es_systems.xml"),
                                          "<systemList/>\n")
                .has_value());
    CHECK(rv::esde::bundledResource(profile, kSystems) ==
          fixture.path("ES-DE/resources/systems/windows/es_systems.xml"));
    CHECK_FALSE(rv::esde::bundledResource(profile, "systems/windows/es_import_rules.xml").has_value());
    profile.installFolder.reset();
    profile.esdeHome = fixture.path("Nowhere");
    CHECK_FALSE(rv::esde::bundledResource(profile, kSystems).has_value());
}

TEST_CASE("esde/discovery: whether a folder is there", "[esde][discovery]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    CHECK(rv::esde::checkFolder(fixture.path("roms")) == rv::esde::FolderCheck{});
    const auto missing = rv::esde::checkFolder(fixture.path("roms/nes"));
    CHECK(missing.availability == rv::catalog::Availability::SourceUnavailable);
    CHECK(missing.reason == rv::catalog::SourceUnavailableReason::FolderMissing);
    CHECK_FALSE(missing.detail.empty());
    const auto file = rv::esde::checkFolder(fixture.path("roms/gb/Notes.txt"));
    CHECK(file.reason == rv::catalog::SourceUnavailableReason::FolderMissing);
    CHECK(file.detail == "a file, not a folder");
    if (const auto drive = unusedDrive()) {
        const auto unreachable = rv::esde::checkFolder(fs::path(std::wstring(1, static_cast<wchar_t>(*drive)) +
                                                                L":\\ROMs"));
        CHECK(unreachable.availability == rv::catalog::Availability::SourceUnavailable);
        CHECK(unreachable.reason == rv::catalog::SourceUnavailableReason::Unreachable);
    }
}

TEST_CASE("esde/discovery: library.json is written and read back; what it may not hold is refused",
          "[esde][discovery][library]") {
    const rv::test::TempDir dir("rv-esde-library");
    const fs::path file = dir.file("config/library.json");

    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("my-games").value();
    profile.esdeHome = fs::path(L"C:\\Users\\Someone\\ES-DE");
    profile.installFolder = fs::path(L"D:\\Programs\\ES-DE");
    profile.romRoot = fs::path(L"\\\\nas\\roms");
    REQUIRE(rv::esde::writeLibraryFile(file, profile).has_value());
    const auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    CHECK(text->starts_with("// The library Retroverse reads"));
    CHECK(text->find("\"schemaVersion\": 1") != std::string::npos);
    CHECK(text->find("\"mediaRoot\": null") != std::string::npos);
    const auto read = rv::esde::readLibraryFile(file);
    INFO((read ? "read" : read.error().toString()));
    REQUIRE(read.has_value());
    CHECK(*read == profile);

    // Hand-written: comments, optional keys left out, folders tidied.
    const auto write = [&](std::string_view json) {
        REQUIRE(rv::core::writeTextFileAtomic(file, json).has_value());
        return rv::esde::readLibraryFile(file);
    };
    const auto written = write(R"(// mine
{
  "schemaVersion": 1, // the format
  "id": "esde",
  "adapter": "esde",
  "esdeHome": "C:/Users/Someone/./ES-DE/"
})");
    REQUIRE(written.has_value());
    CHECK(written->esdeHome == fs::path(L"C:\\Users\\Someone\\ES-DE"));
    CHECK_FALSE(written->installFolder.has_value());

    struct Refused {
        std::string_view json;
        rv::ErrorCode code;
    };
    for (const Refused& refused : {
             Refused{R"({"schemaVersion": 1, "id": "esde", "adapter": "esde", "esdeHome": "C:\\x", "extra": 1})",
                     rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 1, "id": "esde", "adapter": "esde"})", rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 1, "id": "esde", "adapter": "esde", "esdeHome": "ES-DE"})",
                     rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 1, "id": "esde", "adapter": "esde", "esdeHome": ""})",
                     rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 1, "id": "Bad Id", "adapter": "esde", "esdeHome": "C:\\x"})",
                     rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 1, "id": "esde", "adapter": "other", "esdeHome": "C:\\x"})",
                     rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 1, "id": "esde", "adapter": "esde", "esdeHome": "C:\\x", "romRoot": 3})",
                     rv::ErrorCode::InvalidArgument},
             Refused{R"({"schemaVersion": 2, "id": "esde", "adapter": "esde", "esdeHome": "C:\\x"})",
                     rv::ErrorCode::Unsupported},
             Refused{R"({"id": "esde", "adapter": "esde", "esdeHome": "C:\\x"})", rv::ErrorCode::InvalidArgument},
             Refused{R"([1, 2])", rv::ErrorCode::ParseError},
             Refused{R"({"schemaVersion": )", rv::ErrorCode::ParseError},
         }) {
        INFO(refused.json);
        const auto result = write(refused.json);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == refused.code);
    }
    const auto none = rv::esde::readLibraryFile(dir.file("config/none.json"));
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == rv::ErrorCode::NotFound);
}
