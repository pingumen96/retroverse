// es_settings.xml (I2 step 2): the whitelisted keys read as ES-DE 3.4.1 reads them, never the scraper credentials;
// the ROM and media roots with '~', %ESPATH%, empty values and overrides, on the standard and custom-dirs fixtures and
// on files written at test time.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/identity.hpp>
#include <rv/esde/settings.hpp>

#include "support/fixture.hpp"
#include "support/log_capture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::esde::EsdeSettings;
using rv::esde::ResolvedRoot;
using rv::esde::RootSource;

namespace {

rv::test::Fixture materialize(std::string_view name) {
    auto fixture = rv::test::Fixture::materialize(name);
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    return std::move(*fixture);
}

EsdeSettings settingsOf(const fs::path& esdeHome) {
    auto settings = rv::esde::readSettings(esdeHome);
    INFO((settings ? "read" : settings.error().toString()));
    REQUIRE(settings.has_value());
    return std::move(*settings);
}

EsdeSettings settingsFile(const rv::test::TempDir& dir, std::string_view content) {
    const fs::path file = dir.file("es_settings.xml");
    REQUIRE(rv::core::writeTextFileAtomic(file, content).has_value());
    auto settings = rv::esde::readSettingsFile(file);
    INFO((settings ? "read" : settings.error().toString()));
    REQUIRE(settings.has_value());
    return std::move(*settings);
}

/// The ES-DE layout of a fixture: its ES-DE home and install folder.
rv::catalog::LibraryProfile profileOf(const rv::test::Fixture& fixture, std::string_view install) {
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture.path("ES-DE");
    profile.installFolder = fixture.path(install);
    return profile;
}

/// Every text the settings and the roots hold, to look for what must not be there.
std::string everything(const EsdeSettings& settings, const std::vector<ResolvedRoot>& roots = {}) {
    std::string all = rv::core::pathToUtf8(settings.file) + "|" + settings.romDirectory + "|" +
                      settings.mediaDirectory + "|" + settings.theme;
    const auto addIssues = [&](const std::vector<rv::catalog::Issue>& issues) {
        for (const auto& issue : issues) {
            all += "|" + issue.code + "|" + issue.system + "|" + issue.file + "|" + issue.reason;
        }
    };
    addIssues(settings.issues);
    for (const ResolvedRoot& root : roots) {
        all += "|" + root.configured + "|" + root.esdeText;
        if (root.path) {
            all += "|" + rv::core::pathToUtf8(*root.path);
        }
        addIssues(root.issues);
    }
    return all;
}

bool hasIssue(const std::vector<rv::catalog::Issue>& issues, std::string_view code) {
    for (const auto& issue : issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("esde/settings: the standard fixture's keys and roots, and nothing of its scraper credentials",
          "[esde][settings]") {
    const rv::test::LogCapture log;
    spdlog::default_logger()->set_level(spdlog::level::trace);
    const rv::test::Fixture fixture = materialize("esde/standard");
    const std::string root = rv::core::pathToUtf8(fixture.root());

    const EsdeSettings settings = settingsOf(fixture.path("ES-DE"));
    CHECK(settings.source == rv::esde::SettingsSource::File);
    CHECK(settings.file == fixture.path("ES-DE") / "settings" / "es_settings.xml");
    CHECK(settings.romDirectory == root + "\\roms");
    CHECK(settings.mediaDirectory == root + "/media");
    CHECK(settings.showHiddenFiles);
    CHECK(settings.showHiddenGames);
    CHECK_FALSE(settings.parseGamelistOnly);
    CHECK_FALSE(settings.legacyGamelistFileLocation);
    CHECK(settings.alternativeEmulatorPerGame);
    CHECK(settings.uiMode == rv::esde::UiMode::Full);
    CHECK(settings.issues.empty());

    const auto profile = profileOf(fixture, "Programs/ES-DE");
    const ResolvedRoot romRoot = rv::esde::resolveRomRoot(settings, profile);
    REQUIRE(romRoot.path.has_value());
    CHECK(*romRoot.path == fixture.path("roms"));
    CHECK(romRoot.source == RootSource::Setting);
    CHECK(romRoot.configured == root + "\\roms");
    CHECK(romRoot.esdeText == root + "\\roms\\"); // FileData::getROMDirectory() ends with a separator
    CHECK(romRoot.issues.empty());
    const ResolvedRoot mediaRoot = rv::esde::resolveMediaRoot(settings, profile);
    REQUIRE(mediaRoot.path.has_value());
    CHECK(*mediaRoot.path == fixture.path("media"));
    CHECK(mediaRoot.source == RootSource::Setting);
    CHECK(mediaRoot.esdeText == root + "/media/");

    // The credentials are never read: in nothing the reader returns, in nothing it logs.
    for (const std::string_view secret : {"fixture-scraper-user-Q7x4", "fixture-scraper-password-K9w2"}) {
        CHECK(everything(settings, {romRoot, mediaRoot}).find(secret) == std::string::npos);
        CHECK(log.count(secret) == 0);
    }
}

TEST_CASE("esde/settings: only the keys of the whitelist are read, as ES-DE reads them", "[esde][settings]") {
    const rv::test::TempDir dir("rv-esde-settings");
    std::vector<std::string_view> names;
    for (const auto& key : rv::esde::settingKeys()) {
        names.push_back(key.name);
    }
    CHECK(names == std::vector<std::string_view>{"AlternativeEmulatorPerGame", "LegacyGamelistFileLocation",
                                                 "MAMENameStripExtraInfo", "MediaDirectory", "ParseGamelistOnly",
                                                 "ROMDirectory", "ShowHiddenFiles", "ShowHiddenGames", "Theme",
                                                 "UIMode"});

    SECTION("values ES-DE reads otherwise than written, repeated keys, keys of another type") {
        const rv::test::LogCapture log;
        const EsdeSettings settings = settingsFile(dir, R"(<?xml version="1.0"?>
<bool name="ShowHiddenFiles" value="Yes" />
<bool name="ShowHiddenGames" value="maybe" />
<bool name="ParseGamelistOnly" />
<bool name="LegacyGamelistFileLocation" value="1" />
<bool name="AlternativeEmulatorPerGame" value="false" />
<bool name="AlternativeEmulatorPerGame" value="true" />
<int name="ROMDirectory" value="7" />
<string name="ShowHiddenFiles" value="false" />
<string name="ROMDirectory" value="D:\Games &amp; More\ROMs" />
<string name="MediaDirectory" />
<string name="UIMode" value="Kiosk" />
<string name="ScraperPasswordScreenScraper" value="whitelist-secret-&#0;-Z1" />
<float name="UIMode" value="1.5" />
<settings><bool name="ShowHiddenGames" value="true" /></settings>
)");
        CHECK(settings.showHiddenFiles);             // "Yes": 'Y' reads as true
        CHECK_FALSE(settings.showHiddenGames);       // "maybe": false
        CHECK_FALSE(settings.parseGamelistOnly);     // no value: false
        CHECK(settings.legacyGamelistFileLocation);  // "1": true
        CHECK(settings.alternativeEmulatorPerGame);  // the last one counts
        CHECK(settings.romDirectory == R"(D:\Games & More\ROMs)");
        CHECK(settings.mediaDirectory.empty());
        CHECK(settings.uiMode == rv::esde::UiMode::Full); // "Kiosk" is no mode of ES-DE's: full
        std::vector<std::string> codes;
        for (const auto& issue : settings.issues) {
            codes.push_back(issue.code);
            CHECK(issue.file == rv::core::pathToUtf8(dir.file("es_settings.xml")));
        }
        CHECK(codes == std::vector<std::string>{"setting-malformed", "setting-malformed", "setting-malformed",
                                                "setting-malformed", "setting-repeated", "setting-wrong-type",
                                                "setting-wrong-type", "setting-malformed", "setting-malformed",
                                                "setting-wrong-type"});
        CHECK(everything(settings).find("whitelist-secret") == std::string::npos);
        CHECK(log.count("whitelist-secret") == 0);
    }
    SECTION("Theme: the active theme's folder name, read as written (I11 step 1)") {
        const EsdeSettings settings = settingsFile(dir, R"(<?xml version="1.0"?>
<string name="Theme" value="fixture-theme-es-de" />
<string name="ThemeAspectRatio" value="automatic" />
<string name="ScraperUsernameScreenScraper" value="theme-secret-user" />
)");
        CHECK(settings.theme == "fixture-theme-es-de");
        CHECK(rv::esde::activeThemeName(settings) == "fixture-theme-es-de");
        CHECK(settings.issues.empty());
        CHECK(everything(settings).find("theme-secret") == std::string::npos);
        CHECK(rv::esde::activeThemeName(EsdeSettings{}) == "linear-es-de");
    }
    SECTION("the defaults of a file that sets nothing Retroverse reads") {
        const EsdeSettings settings = settingsFile(dir, "<?xml version=\"1.0\"?>\n<int name=\"MaxPlayTimeTracking\" "
                                                        "value=\"8\" />\n");
        CHECK(settings.source == rv::esde::SettingsSource::File);
        CHECK(settings.romDirectory.empty());
        CHECK(settings.mediaDirectory.empty());
        CHECK(settings.showHiddenFiles);
        CHECK(settings.showHiddenGames);
        CHECK_FALSE(settings.parseGamelistOnly);
        CHECK_FALSE(settings.legacyGamelistFileLocation);
        CHECK(settings.alternativeEmulatorPerGame);
        CHECK(settings.uiMode == rv::esde::UiMode::Full);
        CHECK(settings.theme.empty());
        CHECK(settings.issues.empty());
    }
}

TEST_CASE("esde/settings: no file gives ES-DE's defaults, the legacy file is read, a broken file is an error",
          "[esde][settings]") {
    const rv::test::TempDir dir("rv-esde-settings-files");
    const fs::path home = dir.file("ES-DE");
    REQUIRE(rv::core::writeTextFileAtomic(home / "gamelists" / "keep.txt", "").has_value());

    const EsdeSettings defaults = settingsOf(home);
    CHECK(defaults.source == rv::esde::SettingsSource::Defaults);
    CHECK(defaults.file == home / "settings" / "es_settings.xml");
    CHECK(defaults.showHiddenFiles);
    CHECK(defaults.romDirectory.empty());
    CHECK(hasIssue(defaults.issues, "settings-missing"));

    REQUIRE(rv::core::writeTextFileAtomic(home / "es_settings.xml",
                                          "<?xml version=\"1.0\"?>\n<string name=\"UIMode\" value=\"kid\" />\n")
                .has_value());
    const EsdeSettings legacy = settingsOf(home);
    CHECK(legacy.source == rv::esde::SettingsSource::LegacyFile);
    CHECK(legacy.file == home / "es_settings.xml");
    CHECK(legacy.uiMode == rv::esde::UiMode::Kid);
    CHECK(hasIssue(legacy.issues, "settings-legacy-file"));

    // The file of settings/ wins over the legacy one, as ES-DE moves the legacy file only when there is none.
    const fs::path file = home / "settings" / "es_settings.xml";
    REQUIRE(rv::core::writeTextFileAtomic(file, "<?xml version=\"1.0\"?>\n<string name=\"UIMode\" value=\"kiosk\" />\n")
                .has_value());
    CHECK(settingsOf(home).uiMode == rv::esde::UiMode::Kiosk);

    for (const std::string_view broken : {std::string_view("<?xml version=\"1.0\"?>\n<bool name=\"ShowHiddenFiles\" "
                                                           "value=\"true\""),
                                          std::string_view(""), std::string_view("<?xml version=\"1.0\"?>\n")}) {
        REQUIRE(rv::core::writeTextFileAtomic(file, broken).has_value());
        const auto read = rv::esde::readSettings(home);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().code == rv::ErrorCode::ParseError);
        CHECK(read.error().context == rv::core::pathToUtf8(file));
    }
}

TEST_CASE("esde/settings: '~', %ESPATH%, empty values and overrides resolve the roots as ES-DE does",
          "[esde][settings]") {
    const rv::test::LogCapture log;
    spdlog::default_logger()->set_level(spdlog::level::trace);
    const rv::test::Fixture fixture = materialize("esde/custom-dirs");
    const auto profile = profileOf(fixture, "Programs/ES-DE");
    const std::string home = rv::esde::esdeGenericText(fixture.root());
    const std::string install = rv::esde::esdeGenericText(fixture.path("Programs/ES-DE"));

    const EsdeSettings settings = settingsOf(fixture.path("ES-DE"));
    CHECK_FALSE(settings.showHiddenFiles);
    CHECK_FALSE(settings.alternativeEmulatorPerGame);
    CHECK(settings.uiMode == rv::esde::UiMode::Kid);
    CHECK(settings.issues.empty());

    const ResolvedRoot romRoot = rv::esde::resolveRomRoot(settings, profile);
    REQUIRE(romRoot.path.has_value());
    CHECK(*romRoot.path == fixture.path("Games/ROM Library"));
    CHECK(romRoot.source == RootSource::Setting);
    CHECK(romRoot.configured == "~/Games/ROM Library");
    CHECK(romRoot.esdeText == home + "/Games/ROM Library\\");
    CHECK(romRoot.issues.empty());
    const ResolvedRoot mediaRoot = rv::esde::resolveMediaRoot(settings, profile);
    REQUIRE(mediaRoot.path.has_value());
    CHECK(*mediaRoot.path == fixture.path("Programs/Media Store")); // %ESPATH%\.. resolved
    CHECK(mediaRoot.esdeText == install + "\\..\\Media Store/");
    for (const ResolvedRoot* root : {&romRoot, &mediaRoot}) {
        CHECK(rv::esde::checkFolder(*root->path).availability == rv::catalog::Availability::Available);
    }
    // Not even the UI mode's passkey, which is no key of the whitelist, is read.
    for (const std::string_view secret : {"custom-dirs-scraper-user-W8k3", "custom-dirs-scraper-password-R2d7",
                                          "uuddlrlrba"}) {
        CHECK(everything(settings, {romRoot, mediaRoot}).find(secret) == std::string::npos);
        CHECK(log.count(secret) == 0);
    }

    SECTION("empty values are ES-DE's defaults: <home folder>\\ROMs and <ES-DE home>\\downloaded_media") {
        REQUIRE(fixture.replaceText("ES-DE/settings/es_settings.xml", R"(value="~/Games/ROM Library")", R"(value="")"));
        REQUIRE(fixture.replaceText("ES-DE/settings/es_settings.xml", R"(value="%ESPATH%\..\Media Store")",
                                    R"(value="")"));
        const EsdeSettings empty = settingsOf(fixture.path("ES-DE"));
        const ResolvedRoot defaultRom = rv::esde::resolveRomRoot(empty, profile);
        REQUIRE(defaultRom.path.has_value());
        CHECK(*defaultRom.path == fixture.path("ROMs"));
        CHECK(defaultRom.source == RootSource::Default);
        CHECK(defaultRom.configured.empty());
        CHECK(defaultRom.esdeText == home + "/ROMs/");
        const ResolvedRoot defaultMedia = rv::esde::resolveMediaRoot(empty, profile);
        REQUIRE(defaultMedia.path.has_value());
        CHECK(*defaultMedia.path == fixture.path("ES-DE/downloaded_media"));
        CHECK(defaultMedia.source == RootSource::Default);
        for (const ResolvedRoot* root : {&defaultRom, &defaultMedia}) {
            CHECK(rv::esde::checkFolder(*root->path).availability == rv::catalog::Availability::Available);
            CHECK(root->issues.empty());
        }
    }
    SECTION("%ESPATH% without an install folder, a relative folder, '~' inside, a network share") {
        rv::catalog::LibraryProfile noInstall = profile;
        noInstall.installFolder.reset();
        const ResolvedRoot unresolved = rv::esde::resolveMediaRoot(settings, noInstall);
        CHECK_FALSE(unresolved.path.has_value());
        CHECK(unresolved.esdeText.empty());
        CHECK(hasIssue(unresolved.issues, "root-unresolved"));

        EsdeSettings edited = settings;
        edited.romDirectory = "Games\\ROMs";
        CHECK_FALSE(rv::esde::resolveRomRoot(edited, profile).path.has_value());
        CHECK(hasIssue(rv::esde::resolveRomRoot(edited, profile).issues, "root-unresolved"));

        edited.romDirectory = "~/Games/~Old";
        const ResolvedRoot tilde = rv::esde::resolveRomRoot(edited, profile);
        CHECK(hasIssue(tilde.issues, "root-tilde-inside"));
        CHECK(tilde.esdeText == home + "/Games/" + home + "Old\\"); // every '~' replaced, as ES-DE replaces it

        edited.romDirectory = R"(\\nas\roms)";
        const ResolvedRoot share = rv::esde::resolveRomRoot(edited, profile);
        REQUIRE(share.path.has_value());
        CHECK(*share.path == fs::path(LR"(\\nas\roms)"));
        CHECK(hasIssue(share.issues, "root-network-share"));
    }
    SECTION("overrides replace the settings") {
        rv::catalog::LibraryProfile overridden = profile;
        overridden.romRoot = fixture.path("Elsewhere");
        overridden.mediaRoot = fs::path(L"D:/Media//Store/");
        const ResolvedRoot rom = rv::esde::resolveRomRoot(settings, overridden);
        REQUIRE(rom.path.has_value());
        CHECK(*rom.path == fixture.path("Elsewhere"));
        CHECK(rom.source == RootSource::Override);
        CHECK(rom.configured.empty());
        CHECK(rom.esdeText == rv::core::pathToUtf8(fixture.path("Elsewhere").lexically_normal()) + "\\");
        const ResolvedRoot media = rv::esde::resolveMediaRoot(settings, overridden);
        REQUIRE(media.path.has_value());
        CHECK(*media.path == fs::path(L"D:\\Media\\Store"));
        CHECK(media.source == RootSource::Override);
    }
}

TEST_CASE("esde/settings: ES-DE's texts of folders and its replacements", "[esde][settings]") {
    using rv::esde::folderFromEsdeText;
    CHECK(folderFromEsdeText("C:/Games//ROMs/") == fs::path(L"C:\\Games\\ROMs"));
    CHECK(folderFromEsdeText("C:\\") == fs::path(L"C:\\"));
    CHECK(folderFromEsdeText("C:/") == fs::path(L"C:\\"));
    CHECK(folderFromEsdeText(R"(\\nas\share\roms\)") == fs::path(LR"(\\nas\share\roms)"));
    CHECK(folderFromEsdeText("//nas/share") == fs::path(LR"(\\nas\share)"));
    CHECK(folderFromEsdeText(R"(\\?\D:\ROMs)") == fs::path(L"D:\\ROMs"));
    CHECK(folderFromEsdeText(R"(D:\a\..\b\.\c)") == fs::path(L"D:\\b\\c"));
    CHECK(folderFromEsdeText("E:/Giochi/Emulatori\\\\gb") == fs::path(L"E:\\Giochi\\Emulatori\\gb"));
    for (const std::string_view relative : {"ROMs", "C:ROMs", "\\ROMs", "", "\xC3("}) {
        CHECK_FALSE(folderFromEsdeText(relative).has_value());
    }

    CHECK(rv::esde::esdeGenericText(fs::path(L"C:\\Users\\x\\")) == "C:/Users/x");
    CHECK(rv::esde::esdeGenericText(fs::path(L"C:\\")) == "C:");
    CHECK(rv::esde::esdeGenericText(fs::path(L"\\\\?\\C:\\a\\\\b")) == "C:/a/b");
    CHECK(rv::esde::esdeGenericText(fs::path(L"\\\\nas\\emu\\\\ES-DE\\")) == "//nas/emu/ES-DE");
    // A home or install folder on a share still gives absolute folders.
    CHECK(folderFromEsdeText(rv::esde::esdeGenericText(fs::path(L"\\\\nas\\emu\\ES-DE")) + "/../ROMs") ==
          fs::path(LR"(\\nas\emu\ROMs)"));

    CHECK(rv::esde::esdeReplace("a~b~", "~", "C:/x") == "aC:/xbC:/x");
    CHECK(rv::esde::esdeReplace("I:/Roms///gb", "//", "/") == "I:/Roms/gb"); // again until none is left
    CHECK(rv::esde::esdeReplace("x", "x", "xx") == "xx");                    // once when `to` holds `from`
    CHECK(rv::esde::esdeReplace("abc", "", "z") == "abc");
}
