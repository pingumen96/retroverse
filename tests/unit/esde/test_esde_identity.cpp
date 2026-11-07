// The systems' identity (I11 step 1, ADR-0019 §2): ES-DE's theme folders, linear-es-de's metadata files, the logos of
// the themes' layouts, identity.json, and the order each field is resolved in, on the esde/theme fixture (invented
// systems, original logos) and on files written at test time.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/system.hpp>
#include <rv/catalog/system_identity.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/identity.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/themes.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::catalog::IdentitySource;
using rv::catalog::LogoFormat;
using rv::catalog::SystemIdentity;

namespace {

rv::test::Fixture materialize() {
    auto fixture = rv::test::Fixture::materialize("esde/theme");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    return std::move(*fixture);
}

rv::catalog::LibraryProfile profileOf(const rv::test::Fixture& fixture) {
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture.path("ES-DE");
    profile.installFolder = fixture.path("Programs/ES-DE");
    return profile;
}

fs::path linear(const rv::test::Fixture& fixture) {
    return fixture.path("Programs/ES-DE/themes/linear-es-de");
}

rv::catalog::SystemInfo systemInfo(std::string_view name, std::string fullName, std::string theme = {}) {
    rv::catalog::SystemInfo info;
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    info.name = std::move(*parsed);
    info.fullName = std::move(fullName);
    info.theme = theme.empty() ? std::string(name) : std::move(theme);
    return info;
}

std::vector<std::string> codesOf(const std::vector<rv::catalog::Issue>& issues) {
    std::vector<std::string> codes;
    for (const auto& issue : issues) {
        codes.push_back(issue.code);
    }
    return codes;
}

bool holds(const std::vector<rv::catalog::Issue>& issues, std::string_view code) {
    for (const auto& issue : issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

rv::esde::IdentityConfig configFile(const rv::test::TempDir& dir, std::string_view text) {
    const fs::path file = dir.file("identity.json");
    REQUIRE(rv::core::writeTextFileAtomic(file, text).has_value());
    auto config = rv::esde::readIdentityFile(file);
    INFO((config ? "read" : config.error().toString()));
    REQUIRE(config.has_value());
    return std::move(*config);
}

rv::ErrorCode configError(const rv::test::TempDir& dir, std::string_view text) {
    const fs::path file = dir.file("identity.json");
    REQUIRE(rv::core::writeTextFileAtomic(file, text).has_value());
    auto config = rv::esde::readIdentityFile(file);
    REQUIRE_FALSE(config.has_value());
    return config.error().code;
}

std::string jsonPath(const fs::path& path) {
    std::string text = rv::core::pathToUtf8(path);
    std::string escaped;
    for (const char c : text) {
        if (c == '\\') {
            escaped += "\\\\";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

} // namespace

TEST_CASE("esde/themes: theme ids, theme folders and finding a theme", "[esde][identity]") {
    CHECK(rv::esde::isThemeId("snes"));
    CHECK(rv::esde::isThemeId("snes-msu1"));
    CHECK(rv::esde::isThemeId("3do"));
    CHECK(rv::esde::isThemeId("linear-es-de"));
    CHECK(rv::esde::isThemeId("a.b_c"));
    CHECK_FALSE(rv::esde::isThemeId(""));
    CHECK_FALSE(rv::esde::isThemeId("."));
    CHECK_FALSE(rv::esde::isThemeId(".."));
    CHECK_FALSE(rv::esde::isThemeId(".hidden"));
    CHECK_FALSE(rv::esde::isThemeId("../escape"));
    CHECK_FALSE(rv::esde::isThemeId("a/b"));
    CHECK_FALSE(rv::esde::isThemeId("a\\b"));
    CHECK_FALSE(rv::esde::isThemeId("C:x"));
    CHECK_FALSE(rv::esde::isThemeId("with space"));
    CHECK_FALSE(rv::esde::isThemeId(std::string(65, 'a')));
    CHECK_FALSE(rv::esde::isThemeId("caf\xC3\xA9"));

    const rv::test::Fixture fixture = materialize();
    const auto profile = profileOf(fixture);
    const auto folders = rv::esde::themeFolders(profile);
    REQUIRE(folders.size() == 2);
    CHECK(folders[0] == fixture.path("ES-DE") / "themes");
    CHECK(folders[1] == fixture.path("Programs/ES-DE") / "themes");

    rv::catalog::LibraryProfile homeOnly = profile;
    homeOnly.installFolder.reset();
    CHECK(rv::esde::themeFolders(homeOnly).size() == 1);

    CHECK(rv::esde::findTheme(folders, "linear-es-de") == linear(fixture));
    CHECK(rv::esde::findTheme(folders, "chosen-fixture-es-de") == fixture.path("ES-DE/themes/chosen-fixture-es-de"));
    CHECK_FALSE(rv::esde::findTheme(folders, "missing-es-de"));
    CHECK_FALSE(rv::esde::findTheme(folders, "../themes"));
}

TEST_CASE("esde/themes: linear-es-de's metadata files", "[esde][identity]") {
    const rv::test::Fixture fixture = materialize();
    const fs::path theme = linear(fixture);

    SECTION("every value, the Italian description over two lines, the colours") {
        const auto file = rv::esde::themeMetadataFile(theme, "snes");
        REQUIRE(file);
        auto metadata = rv::esde::readThemeMetadata(*file);
        REQUIRE(metadata.has_value());
        CHECK(metadata->name == "Lumen 16");
        CHECK(metadata->manufacturer == "Fixture Works");
        CHECK(metadata->releaseYear == "1991");
        CHECK(metadata->releaseDate == "1991-11-21");
        // The default variables' value: a language's own systemHardwareType is not taken.
        CHECK(metadata->hardwareType == "Console");
        CHECK(metadata->description ==
              "La Lumen 16 \xC3\xA8 una console domestica inventata per i test. Non \xC3\xA8 mai esistita.");
        CHECK(metadata->descriptionLanguage == "it_IT");
        CHECK(metadata->colors == std::vector<std::string>{"#DF5142", "#FED01B", "#0A2A8D", "#007544"});
        CHECK(codesOf(metadata->issues) == std::vector<std::string>{"theme-color-invalid"});

        auto english = rv::esde::readThemeMetadata(*file, "fr_FR");
        REQUIRE(english.has_value());
        CHECK(english->description == "The Lumen 16 is an invented home console of the fixtures. It never existed.");
        CHECK(english->descriptionLanguage.empty());
    }
    SECTION("whitespace collapsed; a year that is no year and a control character left out") {
        const auto file = rv::esde::themeMetadataFile(theme, "megadrive");
        REQUIRE(file);
        auto metadata = rv::esde::readThemeMetadata(*file);
        REQUIRE(metadata.has_value());
        CHECK(metadata->name == "Orbit Drive");
        CHECK(metadata->releaseYear.empty());
        CHECK(metadata->hardwareType.empty());
        CHECK(metadata->description == "The Orbit Drive is invented.");
        CHECK(metadata->descriptionLanguage.empty());
        CHECK(metadata->colors.empty());
        CHECK(codesOf(metadata->issues) == std::vector<std::string>{"theme-value-invalid", "theme-value-invalid"});
    }
    SECTION("a file cut short, a file without <theme>, a file too large, a theme id that names no file") {
        const auto broken = rv::esde::themeMetadataFile(theme, "broken");
        REQUIRE(broken);
        auto metadata = rv::esde::readThemeMetadata(*broken);
        REQUIRE_FALSE(metadata.has_value());
        CHECK(metadata.error().code == rv::ErrorCode::ParseError);

        const rv::test::TempDir dir("rv-esde-theme-metadata");
        const fs::path other = dir.file("other.xml");
        REQUIRE(rv::core::writeTextFileAtomic(other, "<variables><systemName>X</systemName></variables>").has_value());
        auto noTheme = rv::esde::readThemeMetadata(other);
        REQUIRE_FALSE(noTheme.has_value());
        CHECK(noTheme.error().code == rv::ErrorCode::ParseError);

        const fs::path large = dir.file("large.xml");
        std::string text = "<theme><variables><systemName>X</systemName></variables><!--";
        text.append(rv::esde::kMaxThemeMetadataBytes, 'x');
        text += "--></theme>";
        REQUIRE(rv::core::writeTextFileAtomic(large, text).has_value());
        auto tooLarge = rv::esde::readThemeMetadata(large);
        REQUIRE_FALSE(tooLarge.has_value());
        CHECK(tooLarge.error().code == rv::ErrorCode::OutOfRange);

        CHECK_FALSE(rv::esde::themeMetadataFile(theme, "n64"));
        CHECK_FALSE(rv::esde::themeMetadataFile(theme, "../linear-es-de/system/metadata/snes"));
    }
    SECTION("a long description is cut at its last whole sentence") {
        const rv::test::TempDir dir("rv-esde-theme-description");
        std::string sentences;
        while (sentences.size() < rv::esde::kMaxThemeDescriptionBytes + 200) {
            sentences += "An invented sentence of the fixture. ";
        }
        const fs::path file = dir.file("long.xml");
        REQUIRE(rv::core::writeTextFileAtomic(file, "<theme><variables><systemDescription>" + sentences +
                                                        "</systemDescription></variables></theme>").has_value());
        auto metadata = rv::esde::readThemeMetadata(file);
        REQUIRE(metadata.has_value());
        CHECK(metadata->description.size() <= rv::esde::kMaxThemeDescriptionBytes);
        CHECK(metadata->description.ends_with("fixture."));
        CHECK(metadata->issues.empty());
    }
}

TEST_CASE("esde/themes: logos in the themes' layouts and formats", "[esde][identity]") {
    const rv::test::Fixture fixture = materialize();
    const fs::path theme = linear(fixture);
    const fs::path active = fixture.path("ES-DE/themes/active-fixture-es-de");

    const auto snes = rv::esde::findThemeLogo(theme, "snes");
    REQUIRE(snes);
    CHECK(snes->file == theme / "system" / "logos" / "snes.svg");
    CHECK(snes->format == LogoFormat::Svg);

    std::vector<rv::catalog::Issue> issues;
    CHECK_FALSE(rv::esde::findThemeLogo(theme, "gb", &issues));
    CHECK(codesOf(issues) == std::vector<std::string>{"logo-too-large"});

    const auto megadrive = rv::esde::findThemeLogo(active, "megadrive");
    REQUIRE(megadrive);
    CHECK(megadrive->file == active / "_inc" / "systems" / "logos" / "megadrive.png");
    CHECK(megadrive->format == LogoFormat::Png);

    const auto n64 = rv::esde::findThemeLogo(active, "n64");
    REQUIRE(n64);
    CHECK(n64->file == active / "n64" / "images" / "logo.svg");

    CHECK_FALSE(rv::esde::findThemeLogo(theme, "missing"));
    CHECK_FALSE(rv::esde::findThemeLogo(active, "../active-fixture-es-de/n64"));

    CHECK(rv::esde::logoFormatOf("a/B.SVG") == LogoFormat::Svg);
    CHECK(rv::esde::logoFormatOf("b.jpeg") == LogoFormat::Jpeg);
    CHECK(rv::esde::logoFormatOf("c.Jpg") == LogoFormat::Jpeg);
    CHECK_FALSE(rv::esde::logoFormatOf("d.webp"));
}

TEST_CASE("esde/identity: identity.json", "[esde][identity]") {
    const rv::test::TempDir dir("rv-esde-identity-file");

    SECTION("every key, with comments") {
        const fs::path logo = dir.file("logo.svg");
        const auto config = configFile(dir, R"({
  // A user's choices.
  "schemaVersion": 1,
  "theme": "chosen-fixture-es-de",
  "themeRoots": [")" + jsonPath(dir.path()) + R"("],
  "activeTheme": false,
  "systems": {"snes": {"name": " Lumen Sedici ", "colors": ["112233", "#aabbcc"], "logo": ")" +
                                               jsonPath(logo) + R"("}}
})");
        CHECK(config.theme == "chosen-fixture-es-de");
        REQUIRE(config.themeRoots.size() == 1);
        CHECK(config.themeRoots[0] == dir.path().lexically_normal());
        CHECK_FALSE(config.activeTheme);
        REQUIRE(config.systems.contains("snes"));
        const auto& snes = config.systems.at("snes");
        CHECK(snes.name == "Lumen Sedici");
        CHECK(snes.colors == std::vector<std::string>{"#112233", "#AABBCC"});
        CHECK(snes.logo == logo);
        CHECK_FALSE(snes.manufacturer);
    }
    SECTION("the defaults") {
        const auto config = configFile(dir, R"({"schemaVersion": 1})");
        CHECK(config == rv::esde::IdentityConfig{});
    }
    SECTION("what is refused") {
        CHECK(configError(dir, R"({"schemaVersion": 2})") == rv::ErrorCode::Unsupported);
        CHECK(configError(dir, R"({})") == rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, R"({"schemaVersion": 1, "colour": true})") == rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, R"({"schemaVersion": 1, "theme": "../x"})") == rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, R"({"schemaVersion": 1, "themeRoots": ["relative\\themes"]})") ==
              rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, R"({"schemaVersion": 1, "systems": {"snes": {"colors": ["red"]}}})") ==
              rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, R"({"schemaVersion": 1, "systems": {"snes": {"logo": "C:\\a\\logo.webp"}}})") ==
              rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, R"({"schemaVersion": 1, "systems": {"snes": {"maker": "x"}}})") ==
              rv::ErrorCode::InvalidArgument);
        CHECK(configError(dir, "[1, 2]") == rv::ErrorCode::ParseError);
        auto missing = rv::esde::readIdentityFile(dir.file("missing.json"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == rv::ErrorCode::NotFound);
    }
}

TEST_CASE("esde/identity: each field from the first source that gives it", "[esde][identity]") {
    const rv::test::Fixture fixture = materialize();
    const auto profile = profileOf(fixture);
    rv::esde::EsdeSettings settings;
    settings.theme = "active-fixture-es-de";
    rv::esde::IdentityConfig config;
    config.theme = "chosen-fixture-es-de";

    const std::vector<rv::catalog::SystemInfo> systems{
        systemInfo("snes", "Lumen 16 Home System"), systemInfo("megadrive", "Orbit Drive System"),
        systemInfo("gb", "Pocket Lumen System"),   systemInfo("n64", "Fixture 64"),
        systemInfo("broken", "Broken System"),     systemInfo("escape", "Escape System", "../escape"),
    };

    SECTION("the themes, then ES-DE's full name") {
        const auto sources = rv::esde::identitySources(profile, &settings, config);
        REQUIRE(sources.metadataThemes.size() == 2);
        CHECK(sources.metadataThemes[0] == fixture.path("ES-DE/themes/chosen-fixture-es-de"));
        CHECK(sources.metadataThemes[1] == linear(fixture));
        CHECK(sources.activeTheme == fixture.path("ES-DE/themes/active-fixture-es-de"));
        CHECK(sources.issues.empty());

        const auto identities = rv::esde::resolveSystemIdentities(systems, sources, config);
        REQUIRE(identities.size() == systems.size());

        const SystemIdentity& snes = identities[0];
        CHECK(snes.system.str() == "snes");
        CHECK(snes.themeId == "snes");
        CHECK(snes.name.value == "Lumen 16");
        CHECK(snes.name.source == IdentitySource::Theme);
        CHECK(snes.name.origin == "linear-es-de");
        CHECK(snes.manufacturer.value == "Fixture Works");
        CHECK(snes.releaseYear.value == "1991");
        CHECK(snes.descriptionLanguage == "it_IT");
        CHECK(snes.colors.size() == 4);
        CHECK(snes.colorsSource == IdentitySource::Theme);
        CHECK(snes.logo.source == IdentitySource::Theme); // linear-es-de's, before the active theme's
        CHECK(snes.logo.file == linear(fixture) / "system" / "logos" / "snes.svg");
        CHECK(codesOf(snes.issues) == std::vector<std::string>{"theme-color-invalid"});
        CHECK(snes.issues[0].system == "snes");

        const SystemIdentity& megadrive = identities[1];
        CHECK(megadrive.name.value == "Orbit Drive");
        CHECK(megadrive.logo.source == IdentitySource::ActiveTheme);
        CHECK(megadrive.logo.origin == "active-fixture-es-de");
        CHECK(megadrive.logo.format == LogoFormat::Png);

        const SystemIdentity& gb = identities[2];
        CHECK(gb.name.value == "Pocket Lumen"); // the chosen theme's, over linear-es-de's
        CHECK(gb.name.origin == "chosen-fixture-es-de");
        CHECK(gb.hardwareType.value == "Handheld");
        CHECK(gb.manufacturer.empty()); // fields are never mixed from two themes
        CHECK(gb.colors == std::vector<std::string>{"#5E8E7A"});
        CHECK(gb.logo.empty());
        CHECK(holds(gb.issues, "logo-too-large"));

        const SystemIdentity& n64 = identities[3];
        CHECK(n64.name.value == "Fixture 64");
        CHECK(n64.name.source == IdentitySource::FullName);
        CHECK(n64.description.empty());
        CHECK(n64.description.source == IdentitySource::None);
        CHECK(n64.logo.file == fixture.path("ES-DE/themes/active-fixture-es-de") / "n64" / "images" / "logo.svg");

        const SystemIdentity& broken = identities[4];
        CHECK(codesOf(broken.issues) == std::vector<std::string>{"theme-metadata-unreadable"});
        CHECK(broken.name.source == IdentitySource::FullName);

        const SystemIdentity& escape = identities[5];
        CHECK(codesOf(escape.issues) == std::vector<std::string>{"theme-id-invalid"});
        CHECK(escape.name.value == "Escape System");
        CHECK(escape.logo.empty());
    }
    SECTION("identity.json's values win; a logo of its that is no file keeps the themes'") {
        const rv::test::TempDir dir("rv-esde-identity-override");
        const fs::path logo = dir.file("lumen.svg");
        REQUIRE(rv::core::writeTextFileAtomic(logo, "<svg xmlns=\"http://www.w3.org/2000/svg\"/>").has_value());
        rv::esde::IdentityOverride snes;
        snes.name = "Lumen Sedici";
        snes.description = "Una descrizione dell'utente.";
        snes.colors = std::vector<std::string>{"#112233"};
        snes.logo = logo;
        config.systems.emplace("snes", snes);
        rv::esde::IdentityOverride megadrive;
        megadrive.logo = dir.file("missing.png");
        config.systems.emplace("megadrive", megadrive);

        const auto sources = rv::esde::identitySources(profile, &settings, config);
        const auto identities = rv::esde::resolveSystemIdentities(systems, sources, config);
        CHECK(identities[0].name.value == "Lumen Sedici");
        CHECK(identities[0].name.source == IdentitySource::Override);
        CHECK(identities[0].name.origin == "identity.json");
        CHECK(identities[0].manufacturer.source == IdentitySource::Theme);
        CHECK(identities[0].description.value == "Una descrizione dell'utente.");
        CHECK(identities[0].descriptionLanguage.empty());
        CHECK(identities[0].colors == std::vector<std::string>{"#112233"});
        CHECK(identities[0].colorsSource == IdentitySource::Override);
        CHECK(identities[0].logo.file == logo);
        CHECK(identities[0].logo.source == IdentitySource::Override);
        CHECK(identities[1].logo.source == IdentitySource::ActiveTheme);
        CHECK(holds(identities[1].issues, "logo-unreadable"));
    }
    SECTION("without the active theme, without an install folder") {
        config.activeTheme = false;
        auto sources = rv::esde::identitySources(profile, &settings, config);
        CHECK_FALSE(sources.activeTheme);
        auto identities = rv::esde::resolveSystemIdentities(systems, sources, config);
        CHECK(identities[1].logo.empty());

        rv::catalog::LibraryProfile homeOnly = profile;
        homeOnly.installFolder.reset();
        config.activeTheme = true;
        sources = rv::esde::identitySources(homeOnly, &settings, config);
        REQUIRE(sources.metadataThemes.size() == 1);
        REQUIRE(sources.issues.size() == 1);
        CHECK(sources.issues[0].code == "theme-not-found");
        CHECK(sources.issues[0].severity == rv::catalog::IssueSeverity::Info);
        identities = rv::esde::resolveSystemIdentities(systems, sources, config);
        CHECK(identities[0].name.source == IdentitySource::FullName);
        CHECK(identities[0].logo.source == IdentitySource::ActiveTheme);
    }
    SECTION("an active theme or a chosen theme that is nowhere") {
        settings.theme = "missing-es-de";
        config.theme = "also-missing-es-de";
        const auto sources = rv::esde::identitySources(profile, &settings, config);
        CHECK(sources.metadataThemes == std::vector<fs::path>{linear(fixture)});
        CHECK_FALSE(sources.activeTheme);
        CHECK(codesOf(sources.issues) == std::vector<std::string>{"theme-not-found", "theme-not-found"});
    }
    SECTION("no settings: the active theme is ES-DE's default, which is linear-es-de itself") {
        const auto sources = rv::esde::identitySources(profile, nullptr, config);
        CHECK_FALSE(sources.activeTheme);
        CHECK(sources.issues.empty());
    }
}
