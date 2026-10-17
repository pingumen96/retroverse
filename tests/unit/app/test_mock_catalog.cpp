// The mock catalog of retroverse.exe --mock-catalog (I2 step A1): invented games with their launch targets and the
// emulator profiles they use, until the ES-DE catalog reaches the application (step 13).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/mock_catalog.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include "support/temp_dir.hpp"

using rv::ErrorCode;
using rv::app::MockCatalog;
using Json = nlohmann::json;

namespace {

const std::filesystem::path kCatalogFile = rv::core::pathFromUtf8("C:/mock/catalog.json");

Json game(const std::string& id, const std::string& title) {
    return Json{{"id", id}, {"title", title}};
}

Json document(Json games, Json profiles = nullptr) {
    Json json{{"schemaVersion", 1}, {"games", std::move(games)}};
    if (!profiles.is_null()) {
        json["profiles"] = std::move(profiles);
    }
    return json;
}

rv::Expected<MockCatalog> parse(const Json& json) {
    return rv::app::parseMockCatalog(json, kCatalogFile);
}

/// The error of parsing `json`, which must fail.
rv::Error errorOf(const Json& json) {
    const auto parsed = parse(json);
    REQUIRE_FALSE(parsed.has_value());
    return parsed.error();
}

Json fakeProfile(const std::string& id) {
    return Json{{"id", id},
                {"displayName", "Emulatore di prova"},
                {"executable", "C:\\emulators\\fake.exe"},
                {"arguments", Json::array({"{rom}"})}};
}

} // namespace

TEST_CASE("app/mock_catalog: games with their entries and launch targets", "[app][mock_catalog]") {
    Json first = game("g000000000000a001", "Lanterna di Vetro");
    first["platform"] = "Portatile 8 bit";
    first["year"] = 1991;
    first["developer"] = "Studio Ventaglio";
    first["publisher"] = "Edizioni Faro";
    first["genre"] = "Rompicapo";
    first["players"] = "1";
    first["description"] = "Una lanterna.";
    first["state"] = "needs-configuration";
    first["launch"] = Json{{"profile", "fake"},
                           {"rom", "roms/lanterna.gb"},
                           {"system", "gb"},
                           {"variables", Json{{"core", "C:\\cores\\a.dll"}}}};
    Json second = game("g000000000000a002", "Il Faro Sommerso");
    second["launch"] = Json{{"profile", "retroarch"}, {"rom", "D:\\Giochi\\faro.gb"}};
    const auto catalog = parse(document(Json::array({first, second, game("g000000000000a003", "Senza avvio")}),
                                        Json::array({fakeProfile("fake")})));
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->games.size() == 3);
    CHECK(catalog->file == kCatalogFile);

    const rv::ui::CatalogEntry& entry = catalog->games[0].entry;
    CHECK(entry.id == "g000000000000a001");
    CHECK(entry.title == "Lanterna di Vetro");
    CHECK(entry.platform == "Portatile 8 bit");
    CHECK(entry.year == 1991);
    CHECK(entry.developer == "Studio Ventaglio");
    CHECK(entry.publisher == "Edizioni Faro");
    CHECK(entry.genre == "Rompicapo");
    CHECK(entry.players == "1");
    CHECK(entry.description == "Una lanterna.");
    CHECK(entry.state == "needs-configuration");

    // A relative ROM is relative to the catalog file's folder; the title names the launch.
    REQUIRE(catalog->games[0].launch.has_value());
    const rv::app::MockLaunch& launch = *catalog->games[0].launch;
    CHECK(launch.profile == "fake");
    CHECK(launch.target.rom == rv::core::pathFromUtf8("C:/mock/roms/lanterna.gb").lexically_normal());
    CHECK(launch.target.rom.is_absolute());
    CHECK(launch.target.system == "gb");
    CHECK(launch.target.title == "Lanterna di Vetro");
    REQUIRE(launch.target.variables.count("core") == 1);
    CHECK(launch.target.variables.at("core") == "C:\\cores\\a.dll");
    // An absolute ROM stays as it is; no system is the empty name.
    CHECK(catalog->games[1].launch->target.rom == rv::core::pathFromUtf8("D:/Giochi/faro.gb").lexically_normal());
    CHECK(catalog->games[1].launch->target.system.empty());
    // A game without a launch target cannot launch.
    CHECK_FALSE(catalog->games[2].launch.has_value());

    // Lookups and the entries in order.
    CHECK(catalog->find("g000000000000a002") == &catalog->games[1]);
    CHECK(catalog->find("g000000000000a999") == nullptr);
    const std::vector<rv::ui::CatalogEntry> entries = catalog->entries();
    REQUIRE(entries.size() == 3);
    CHECK(entries[2].title == "Senza avvio");

    // The catalog's own profiles, as a profile document of its file.
    REQUIRE(catalog->profiles.profiles.size() == 1);
    CHECK(catalog->profiles.profiles[0].id == "fake");
    CHECK(catalog->profiles.profiles[0].origin == rv::launcher::ProfileOrigin::User);
    CHECK(catalog->profiles.issues.empty());
}

TEST_CASE("app/mock_catalog: a malformed catalog is an error that points at the field", "[app][mock_catalog]") {
    CHECK(errorOf(Json::array()).code == ErrorCode::ParseError);
    CHECK(errorOf(Json{{"games", Json::array()}}).code == ErrorCode::ParseError);
    const rv::Error version = errorOf(Json{{"schemaVersion", 2}, {"games", Json::array()}});
    CHECK(version.code == ErrorCode::Unsupported);
    CHECK(version.context == "C:/mock/catalog.json:/schemaVersion");
    CHECK(errorOf(Json{{"schemaVersion", 1}}).code == ErrorCode::ParseError); // no games
    CHECK(errorOf(Json{{"schemaVersion", 1}, {"games", Json::object()}}).context == "C:/mock/catalog.json:/games");

    // Unknown fields, at every level: a misspelled field never passes for a default.
    Json extra = document(Json::array());
    extra["gamez"] = Json::array();
    CHECK(errorOf(extra).context == "C:/mock/catalog.json:/gamez");
    Json misspelled = game("g1", "A");
    misspelled["titel"] = "B";
    CHECK(errorOf(document(Json::array({misspelled}))).context == "C:/mock/catalog.json:/games/0/titel");
    Json launchField = game("g1", "A");
    launchField["launch"] = Json{{"profile", "fake"}, {"rom", "a.gb"}, {"emulator", "x"}};
    CHECK(errorOf(document(Json::array({launchField}))).context == "C:/mock/catalog.json:/games/0/launch/emulator");

    // Ids, titles, years.
    const rv::Error badId = errorOf(document(Json::array({game("g 1", "A")})));
    CHECK(badId.code == ErrorCode::ValidationFailed);
    CHECK(badId.context == "C:/mock/catalog.json:/games/0/id");
    const rv::Error repeated = errorOf(document(Json::array({game("g1", "A"), game("g1", "B")})));
    CHECK(repeated.code == ErrorCode::ValidationFailed);
    CHECK(repeated.context == "C:/mock/catalog.json:/games/1/id");
    CHECK(errorOf(document(Json::array({game("g1", "")}))).context == "C:/mock/catalog.json:/games/0/title");
    CHECK(errorOf(document(Json::array({Json{{"id", "g1"}}}))).code == ErrorCode::ParseError);
    Json year = game("g1", "A");
    year["year"] = 12000;
    CHECK(errorOf(document(Json::array({year}))).context == "C:/mock/catalog.json:/games/0/year");
    year["year"] = "1991";
    CHECK(errorOf(document(Json::array({year}))).context == "C:/mock/catalog.json:/games/0/year");

    // Launch targets.
    Json noRom = game("g1", "A");
    noRom["launch"] = Json{{"profile", "fake"}};
    CHECK(errorOf(document(Json::array({noRom}))).code == ErrorCode::ParseError);
    Json emptyProfile = game("g1", "A");
    emptyProfile["launch"] = Json{{"profile", ""}, {"rom", "a.gb"}};
    CHECK(errorOf(document(Json::array({emptyProfile}))).context == "C:/mock/catalog.json:/games/0/launch/profile");
    Json badVariable = game("g1", "A");
    badVariable["launch"] = Json{{"profile", "fake"}, {"rom", "a.gb"}, {"variables", Json{{"core", 3}}}};
    CHECK(errorOf(document(Json::array({badVariable}))).context ==
          "C:/mock/catalog.json:/games/0/launch/variables/core");
}

TEST_CASE("app/mock_catalog: its profiles go over the shipped and the user's ones", "[app][mock_catalog]") {
    Json broken = fakeProfile("broken");
    broken.erase("arguments");
    const auto catalog = parse(document(Json::array(), Json::array({fakeProfile("retroarch"), fakeProfile("fake"),
                                                                    broken})));
    REQUIRE(catalog.has_value());
    // A broken profile is an issue of the catalog, not an error: the others still load.
    CHECK(catalog->profiles.profiles.size() == 2);
    REQUIRE(catalog->profiles.issues.size() == 1);
    CHECK(catalog->profiles.issues[0].error.context.starts_with("C:/mock/catalog.json:/profiles/2"));

    rv::launcher::EmulatorProfiles base;
    rv::launcher::EmulatorProfile shipped;
    shipped.id = "retroarch";
    shipped.displayName = "RetroArch";
    base.profiles.push_back(shipped);
    rv::launcher::EmulatorProfile other;
    other.id = "mgba";
    base.profiles.push_back(other);
    const rv::launcher::EmulatorProfiles merged = rv::app::mockCatalogProfiles(base, *catalog);
    REQUIRE(merged.profiles.size() == 3);
    // Replaced in place by the catalog's profile with its id, the others kept, the catalog's new ones after.
    CHECK(merged.profiles[0].id == "retroarch");
    CHECK(merged.profiles[0].displayName == "Emulatore di prova");
    CHECK(merged.profiles[1].id == "mgba");
    CHECK(merged.profiles[2].id == "fake");
    CHECK(merged.issues.size() == 1);
}

TEST_CASE("app/mock_catalog: read from a file with comments, relative to its folder", "[app][mock_catalog]") {
    const rv::test::TempDir dir("rv-mock-catalog");
    const std::filesystem::path file = dir.file("catalogo prova/catalog.json");
    REQUIRE(rv::core::writeTextFileAtomic(file, "// invented games\n"
                                                "{\"schemaVersion\": 1, \"games\": [\n"
                                                "  {\"id\": \"g1\", \"title\": \"Lanterna\", \"launch\": "
                                                "{\"profile\": \"fake\", \"rom\": \"roms/l.gb\"}}]}\n")
                .has_value());
    const auto catalog = rv::app::loadMockCatalog(file);
    REQUIRE(catalog.has_value());
    CHECK(catalog->games[0].launch->target.rom == (file.parent_path() / "roms" / "l.gb").lexically_normal());

    const auto missing = rv::app::loadMockCatalog(dir.file("absent.json"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("app/mock_catalog: the smoke tests' fixture is a valid catalog whose games name its profiles",
          "[app][mock_catalog]") {
    // tests/smoke/catalogs/fake_emulator_games.json, as tests/smoke/test_launch.cpp materializes it.
    const std::filesystem::path fixture =
        rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "smoke" / "catalogs" / "fake_emulator_games.json";
    auto json = rv::core::readJsonFile(fixture);
    REQUIRE(json.has_value());
    for (Json& profile : (*json)["profiles"]) {
        const std::string executable = profile["executable"].get<std::string>();
        CHECK((executable.starts_with("@") && executable.ends_with("@")));
        profile["executable"] = "C:\\build\\bin\\rv_fake_emulator.exe";
    }
    const auto catalog = rv::app::parseMockCatalog(*json, rv::core::pathFromUtf8("C:/build/smoke/case/catalog.json"));
    REQUIRE(catalog.has_value());
    for (const rv::launcher::ProfileIssue& issue : catalog->profiles.issues) {
        FAIL_CHECK(issue.toString());
    }
    CHECK(catalog->games.size() >= 10);
    for (const rv::app::MockGame& game : catalog->games) {
        if (game.launch) {
            CAPTURE(game.entry.id);
            bool known = false;
            for (const rv::launcher::EmulatorProfile& profile : catalog->profiles.profiles) {
                known = known || profile.id == game.launch->profile;
            }
            CHECK(known);
        }
    }
}

TEST_CASE("app/mock_catalog: --mock-catalog takes a .json file", "[app][mock_catalog][command_line]") {
    const auto none = rv::app::parseCommandLine(std::vector<std::string>{});
    REQUIRE(none.has_value());
    CHECK_FALSE(none->mockCatalog.has_value());

    const auto given = rv::app::parseCommandLine(std::vector<std::string>{"--mock-catalog", "build/giochi prova.JSON"});
    REQUIRE(given.has_value());
    CHECK(given->mockCatalog == rv::core::pathFromUtf8("build/giochi prova.JSON"));

    const auto wrong = rv::app::parseCommandLine(std::vector<std::string>{"--mock-catalog", "catalog.xml"});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == ErrorCode::InvalidArgument);
    CHECK(wrong.error().context == "--mock-catalog=catalog.xml");
    CHECK_FALSE(rv::app::parseCommandLine(std::vector<std::string>{"--mock-catalog="}).has_value());
    CHECK(rv::app::commandLineHelp().find("  --mock-catalog <json>") != std::string::npos);
}

TEST_CASE("app/mock_catalog: frame-limited runs bound the wait for a launched game (--launch-wait-limit)",
          "[app][mock_catalog][command_line]") {
    using rv::app::CommandLineOptions;
    const auto parse = [](std::vector<std::string> arguments) { return rv::app::parseCommandLine(arguments); };
    // Interactive runs wait as long as the game runs.
    const auto interactive = parse({});
    REQUIRE(interactive.has_value());
    CHECK_FALSE(interactive->launchWaitLimitSeconds.has_value());
    // Frame-limited runs always have a limit, so that no run waits forever for a game that never ends.
    const auto limited = parse({"--exit-after-frames", "--frames", "50"});
    REQUIRE(limited.has_value());
    CHECK(limited->launchWaitLimitSeconds == CommandLineOptions::kDefaultLaunchWaitLimitSeconds);
    const auto given = parse({"--smoke", "--launch-wait-limit", "2.5"});
    REQUIRE(given.has_value());
    CHECK(given->launchWaitLimitSeconds == 2.5);
    CHECK(parse({"--smoke", "--launch-wait-limit=3600"}).has_value());

    const auto withoutLimit = parse({"--launch-wait-limit", "10"});
    REQUIRE_FALSE(withoutLimit.has_value());
    CHECK(withoutLimit.error().code == ErrorCode::InvalidArgument);
    CHECK(withoutLimit.error().context == "--launch-wait-limit");
    for (const char* text : {"0", "-1", "3601", "inf", "nan"}) {
        INFO(text);
        const auto rejected = parse({"--smoke", "--launch-wait-limit", text});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().context.starts_with("--launch-wait-limit="));
    }
    CHECK_FALSE(parse({"--smoke", "--launch-wait-limit", "soon"}).has_value());
    CHECK(rv::app::commandLineHelp().find("  --launch-wait-limit <seconds>") != std::string::npos);
}
