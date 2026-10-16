#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/launcher/emulator_profile.hpp>

#include "support/fake_system_probe.hpp"
#include "support/temp_dir.hpp"

using namespace std::chrono_literals;
using rv::ErrorCode;
using rv::core::RegistryRoot;
using rv::launcher::EmulatorProfile;
using rv::launcher::EmulatorProfiles;
using rv::launcher::EnvironmentOverride;
using rv::launcher::ExecutableSpec;
using rv::launcher::loadEmulatorProfiles;
using rv::launcher::loadProfileDocument;
using rv::launcher::locateExecutable;
using rv::launcher::LocationRule;
using rv::launcher::mergeProfileDocuments;
using rv::launcher::parseProfileDocument;
using rv::launcher::ProfileDocument;
using rv::launcher::ProfileOrigin;
using rv::launcher::WaitStrategy;

namespace {

constexpr std::string_view kAppPaths = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";

nlohmann::json parsed(std::string_view text) {
    const auto json = rv::core::parseJson(text, "test");
    REQUIRE(json.has_value());
    return *json;
}

/// A document with one profile.
ProfileDocument single(const nlohmann::json& profile, ProfileOrigin origin = ProfileOrigin::Shipped) {
    return parseProfileDocument(nlohmann::json{{"schemaVersion", 1}, {"profiles", {profile}}}, "emulators.json",
                                origin);
}

/// The smallest valid profile.
nlohmann::json minimal(std::string id = "emu") {
    return nlohmann::json{{"id", id}, {"executable", R"(C:\Emulators\emu.exe)"}, {"arguments", {"{rom}"}}};
}

/// The issue of a document that holds exactly one.
const rv::launcher::ProfileIssue& onlyIssue(const ProfileDocument& document) {
    INFO((document.issues.empty() ? std::string("no issue") : document.issues.front().toString()));
    REQUIRE(document.issues.size() == 1);
    return document.issues.front();
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/// Environment variables of a test.
rv::launcher::EnvironmentLookup environmentOf(std::map<std::string, std::string> variables) {
    return [variables = std::move(variables)](std::string_view name) -> std::optional<std::string> {
        const auto found = variables.find(std::string(name));
        return found == variables.end() ? std::nullopt : std::optional<std::string>(found->second);
    };
}

} // namespace

TEST_CASE("launcher/emulator_profile: a profile with every field", "[launcher][emulator_profile]") {
    const ProfileDocument document = parseProfileDocument(parsed(R"json({
        // Comments are allowed, as in every configuration file.
        "schemaVersion": 1,
        "profiles": [{
            "id": "retroarch-isolated",
            "displayName": "RetroArch (isolated)",
            "executable": {"appPaths": "retroarch.exe", "paths": ["~\\RetroArch\\retroarch.exe",
                                                                  "{env:PROGRAMFILES}\\RetroArch\\..\\retroarch*.exe"]},
            "arguments": ["-L", "{var:core}", "--appendconfig={var:scratch}\\isolated.cfg", "{rom}"],
            "workingDirectory": "{rom_dir}",
            "environment": {"EMU_HOME": "{var:scratch}", "TEMP": null},
            "wait": "tree",
            "windowTimeoutSeconds": 0.5,
            "hideConsoleWindow": true,
            "variables": {"cores": "{emulator_dir}\\cores", "scratch": "{env:LOCALAPPDATA}\\scratch"},
            "requiredFiles": ["{var:core}"],
            "systems": {"gb": {"variables": {"core": "{var:cores}\\gambatte_libretro.dll"}}, "snes": {}},
            "verified": true
        }]
    })json"),
                                                          "user/emulators.json", ProfileOrigin::User);
    INFO((document.issues.empty() ? std::string() : document.issues.front().toString()));
    REQUIRE(document.issues.empty());
    REQUIRE(document.profiles.size() == 1);
    const EmulatorProfile& profile = document.profiles.front();
    CHECK(profile.id == "retroarch-isolated");
    CHECK(profile.displayName == "RetroArch (isolated)");
    CHECK(profile.executable.appPathsName == "retroarch.exe");
    CHECK(profile.executable.paths == std::vector<std::string>{R"(~\RetroArch\retroarch.exe)",
                                                               R"({env:PROGRAMFILES}\RetroArch\..\retroarch*.exe)"});
    CHECK(profile.arguments ==
          std::vector<std::string>{"-L", "{var:core}", R"(--appendconfig={var:scratch}\isolated.cfg)", "{rom}"});
    CHECK(profile.workingDirectory == "{rom_dir}");
    const std::vector<EnvironmentOverride> environment{{"EMU_HOME", "{var:scratch}"}, {"TEMP", std::nullopt}};
    CHECK(profile.environment == environment);
    CHECK(profile.waitStrategy == WaitStrategy::ProcessTree);
    CHECK(profile.windowTimeout == 500ms);
    CHECK(profile.hideConsoleWindow);
    CHECK(profile.variables.at("cores") == R"({emulator_dir}\cores)");
    CHECK(profile.requiredFiles == std::vector<std::string>{"{var:core}"});
    CHECK(profile.serves("gb"));
    CHECK(profile.serves("snes"));
    CHECK_FALSE(profile.serves("GB")); // ES-DE system names are exact
    REQUIRE(profile.systemVariables("gb") != nullptr);
    CHECK(profile.systemVariables("gb")->at("core") == R"({var:cores}\gambatte_libretro.dll)");
    CHECK(profile.systemVariables("snes")->empty());
    CHECK(profile.systemVariables("nes") == nullptr);
    CHECK(profile.verified);
    CHECK(profile.origin == ProfileOrigin::User);
    CHECK(profile.source == "user/emulators.json");
}

TEST_CASE("launcher/emulator_profile: defaults of the optional fields", "[launcher][emulator_profile]") {
    const ProfileDocument document = single(minimal());
    REQUIRE(document.issues.empty());
    REQUIRE(document.profiles.size() == 1);
    const EmulatorProfile& profile = document.profiles.front();
    CHECK(profile.displayName == "emu");
    CHECK(profile.executable.appPathsName.empty());
    CHECK(profile.executable.paths == std::vector<std::string>{R"(C:\Emulators\emu.exe)"});
    CHECK(profile.workingDirectory.empty());
    CHECK(profile.environment.empty());
    CHECK(profile.waitStrategy == WaitStrategy::DirectProcess);
    CHECK(profile.windowTimeout == 30s);
    CHECK_FALSE(profile.hideConsoleWindow);
    CHECK(profile.variables.empty());
    CHECK(profile.requiredFiles.empty());
    CHECK(profile.systems.empty());
    CHECK_FALSE(profile.verified);
    CHECK(profile.origin == ProfileOrigin::Shipped);

    nlohmann::json noArguments = minimal();
    noArguments["arguments"] = nlohmann::json::array();
    CHECK(single(noArguments).issues.empty()); // an emulator may take no argument at all, if it says so
}

TEST_CASE("launcher/emulator_profile: errors name the file, the place and the profile",
          "[launcher][emulator_profile]") {
    struct Case {
        std::string field;
        nlohmann::json value;
        ErrorCode code;
        std::string pointer;
        std::string named;
    };
    const std::vector<Case> cases{
        {"displayName", 3, ErrorCode::ParseError, "/profiles/0/displayName", "expected a string"},
        {"displayName", "", ErrorCode::ValidationFailed, "/profiles/0/displayName", "non-empty"},
        {"executable", "emu.exe", ErrorCode::ValidationFailed, "/profiles/0/executable", "absolute"},
        {"executable", "C:{env:PROGRAMFILES}\\emu.exe", ErrorCode::ValidationFailed, "/profiles/0/executable",
         "absolute"},
        {"executable", "C:\\{rom_dir}\\emu.exe", ErrorCode::ValidationFailed, "/profiles/0/executable",
         "only {env:NAME} tokens, not {rom_dir}"},
        {"executable", "C:\\Emu*\\emu.exe", ErrorCode::ValidationFailed, "/profiles/0/executable", "last element"},
        {"executable", "{env:PATH}\\emu.exe", ErrorCode::ValidationFailed, "/profiles/0/executable", "{env:PATH}"},
        {"executable", nlohmann::json::object(), ErrorCode::ValidationFailed, "/profiles/0/executable",
         "neither an App Paths name nor a path"},
        {"executable", {{"appPaths", "C:\\RetroArch\\retroarch.exe"}}, ErrorCode::ValidationFailed,
         "/profiles/0/executable/appPaths", "without a folder"},
        {"executable", {{"appPaths", "retro*.exe"}}, ErrorCode::ValidationFailed, "/profiles/0/executable/appPaths",
         "wildcards"},
        {"executable", {{"paths", {"C:\\ok.exe", "relative\\emu.exe"}}}, ErrorCode::ValidationFailed,
         "/profiles/0/executable/paths/1", "absolute"},
        {"executable", {{"path", "C:\\emu.exe"}}, ErrorCode::ParseError, "/profiles/0/executable/path",
         "unknown field \"path\""},
        {"arguments", "-L {rom}", ErrorCode::ParseError, "/profiles/0/arguments", "expected an array"},
        {"arguments", {"-L", "{romz}"}, ErrorCode::ParseError, "/profiles/0/arguments/1", "unknown token {romz}"},
        {"arguments", {"{rom"}, ErrorCode::ParseError, "/profiles/0/arguments/0", "without its '}'"},
        {"arguments", {"{env:GITHUB_TOKEN}"}, ErrorCode::ValidationFailed, "/profiles/0/arguments/0",
         "{env:GITHUB_TOKEN}"},
        {"workingDirectory", "", ErrorCode::ValidationFailed, "/profiles/0/workingDirectory", "non-empty"},
        {"workingDirectory", "{nope}", ErrorCode::ParseError, "/profiles/0/workingDirectory", "unknown token {nope}"},
        {"environment", {{"A=B", "x"}}, ErrorCode::ValidationFailed, "/profiles/0/environment/A=B", "'='"},
        {"environment", {{"Path", "x"}, {"PATH", "y"}}, ErrorCode::ValidationFailed, "/profiles/0/environment/Path",
         "name the same environment variable"},
        {"environment", {{"X", 1}}, ErrorCode::ParseError, "/profiles/0/environment/X", "expected a string"},
        {"wait", "forever", ErrorCode::ValidationFailed, "/profiles/0/wait", "unknown wait strategy \"forever\""},
        {"windowTimeoutSeconds", -1, ErrorCode::ValidationFailed, "/profiles/0/windowTimeoutSeconds", "seconds"},
        {"windowTimeoutSeconds", 86401, ErrorCode::ValidationFailed, "/profiles/0/windowTimeoutSeconds", "seconds"},
        {"windowTimeoutSeconds", "30", ErrorCode::ParseError, "/profiles/0/windowTimeoutSeconds", "number"},
        {"hideConsoleWindow", "yes", ErrorCode::ParseError, "/profiles/0/hideConsoleWindow", "true or false"},
        {"variables", {{"a b", "x"}}, ErrorCode::ValidationFailed, "/profiles/0/variables/a b",
         "invalid variable name"},
        {"variables", {{"core", {"x"}}}, ErrorCode::ParseError, "/profiles/0/variables/core", "expected a string"},
        {"requiredFiles", {""}, ErrorCode::ValidationFailed, "/profiles/0/requiredFiles/0", "non-empty"},
        {"systems", {{"g/b", nlohmann::json::object()}}, ErrorCode::ValidationFailed, "/profiles/0/systems/g~1b",
         "invalid system name"},
        {"systems", {{"gb", {{"core", "x"}}}}, ErrorCode::ParseError, "/profiles/0/systems/gb/core",
         "unknown field \"core\""},
        {"systems", {{"gb", {{"variables", {{"core", "{bad"}}}}}}, ErrorCode::ParseError,
         "/profiles/0/systems/gb/variables/core", "without its '}'"},
        {"verified", 1, ErrorCode::ParseError, "/profiles/0/verified", "true or false"},
        {"extends", "retroarch", ErrorCode::ParseError, "/profiles/0/extends", "unknown field \"extends\""},
    };
    for (const Case& c : cases) {
        nlohmann::json profile = minimal("named-profile");
        profile[c.field] = c.value;
        INFO(c.field << " = " << c.value.dump());
        const ProfileDocument document = single(profile);
        CHECK(document.profiles.empty());
        const auto& issue = onlyIssue(document);
        CHECK(issue.file == "emulators.json");
        CHECK(issue.profileId == "named-profile");
        CHECK(issue.error.code == c.code);
        CHECK(issue.error.context == "emulators.json:" + c.pointer);
        CHECK(issue.error.message.find(c.named) != std::string::npos);
        CHECK(issue.toString().starts_with("profile 'named-profile': "));
    }
}

TEST_CASE("launcher/emulator_profile: ids and required fields", "[launcher][emulator_profile]") {
    for (const std::string id : {"retroarch", "a", "0emu", "my.emu_2-x"}) {
        INFO(id);
        CHECK(single(minimal(id)).issues.empty());
    }
    for (const std::string& id :
         std::vector<std::string>{"", "RetroArch", "-emu", ".emu", "has space", "\xC3\xA9mu", std::string(65, 'a')}) {
        INFO(id);
        const ProfileDocument document = single(minimal(id));
        CHECK(onlyIssue(document).error.code == ErrorCode::ValidationFailed);
        CHECK(onlyIssue(document).profileId.empty()); // an invalid id names nothing
        CHECK(onlyIssue(document).error.context == "emulators.json:/profiles/0/id");
    }
    for (const std::string_view field : {"id", "executable", "arguments"}) {
        INFO(field);
        nlohmann::json profile = minimal();
        profile.erase(std::string(field));
        const ProfileDocument document = single(profile);
        CHECK(onlyIssue(document).error.code == ErrorCode::ParseError);
        CHECK(onlyIssue(document).error.message.find(std::string(field)) != std::string::npos);
        CHECK(onlyIssue(document).error.context == "emulators.json:/profiles/0");
    }
    const ProfileDocument notObject = single(nlohmann::json::array());
    CHECK(onlyIssue(notObject).error.code == ErrorCode::ParseError);
}

TEST_CASE("launcher/emulator_profile: executable paths", "[launcher][emulator_profile]") {
    for (const std::string path : {R"(C:\Emulators\emu.exe)", R"(\\nas\share\emu.exe)", "~", R"(~\emu\emu.exe)",
                                   "~/emu/emu.exe", R"({env:PROGRAMFILES}\Emu\emu.exe)",
                                   R"({env:PROGRAMFILES(X86)}\Emu\emu*.exe)", R"(D:\Emus\{env:USERPROFILE}x.exe)",
                                   R"(C:\Emu\..\Emu\emu?.exe)"}) {
        INFO(path);
        nlohmann::json profile = minimal();
        profile["executable"] = path;
        CHECK(single(profile).issues.empty());
    }
}

TEST_CASE("launcher/emulator_profile: documents", "[launcher][emulator_profile]") {
    SECTION("not an object") {
        const ProfileDocument document = parseProfileDocument(nlohmann::json::array(), "f.json", ProfileOrigin::User);
        CHECK(onlyIssue(document).error.code == ErrorCode::ParseError);
        CHECK(onlyIssue(document).error.context == "f.json");
        CHECK(onlyIssue(document).toString().find("f.json") != std::string::npos);
    }
    SECTION("the version first") {
        const ProfileDocument missing = parseProfileDocument(parsed(R"({"profiles": []})"), "f.json",
                                                             ProfileOrigin::User);
        CHECK(onlyIssue(missing).error.code == ErrorCode::ParseError);
        const ProfileDocument newer = parseProfileDocument(parsed(R"({"schemaVersion": 2, "profiles": 7})"), "f.json",
                                                           ProfileOrigin::User);
        CHECK(onlyIssue(newer).error.code == ErrorCode::Unsupported);
        CHECK(onlyIssue(newer).error.context == "f.json:/schemaVersion");
        const ProfileDocument text = parseProfileDocument(parsed(R"({"schemaVersion": "1"})"), "f.json",
                                                          ProfileOrigin::User);
        CHECK(onlyIssue(text).error.code == ErrorCode::ParseError);
    }
    SECTION("profiles must be an array") {
        CHECK(onlyIssue(parseProfileDocument(parsed(R"({"schemaVersion": 1})"), "f.json", ProfileOrigin::User))
                  .error.context == "f.json:/profiles");
        CHECK(onlyIssue(parseProfileDocument(parsed(R"({"schemaVersion": 1, "profiles": {}})"), "f.json",
                                             ProfileOrigin::User))
                  .error.code == ErrorCode::ParseError);
    }
    SECTION("an unknown top-level field is reported, the profiles still load") {
        nlohmann::json json{{"schemaVersion", 1}, {"profiles", {minimal()}}, {"choices", {{"gb", "emu"}}}};
        const ProfileDocument document = parseProfileDocument(json, "f.json", ProfileOrigin::User);
        CHECK(document.profiles.size() == 1);
        CHECK(onlyIssue(document).error.context == "f.json:/choices");
    }
    SECTION("a broken profile is left out, the others load; a second id is refused") {
        nlohmann::json broken = minimal("broken");
        broken["arguments"] = {"{rom"};
        nlohmann::json json{{"schemaVersion", 1},
                            {"profiles", {minimal("first"), broken, minimal("second"), minimal("first")}}};
        const ProfileDocument document = parseProfileDocument(json, "f.json", ProfileOrigin::User);
        REQUIRE(document.profiles.size() == 2);
        CHECK(document.profiles[0].id == "first");
        CHECK(document.profiles[1].id == "second");
        REQUIRE(document.issues.size() == 2);
        CHECK(document.issues[0].profileId == "broken");
        CHECK(document.issues[0].error.context == "f.json:/profiles/1/arguments/0");
        CHECK(document.issues[1].profileId == "first");
        CHECK(document.issues[1].error.context == "f.json:/profiles/3/id");
        CHECK(document.issues[1].error.message.find("second profile") != std::string::npos);
    }
}

TEST_CASE("launcher/emulator_profile: user profiles replace shipped ones by id and add new ones",
          "[launcher][emulator_profile]") {
    nlohmann::json shippedEmu = minimal("emu");
    shippedEmu["displayName"] = "Shipped Emu";
    nlohmann::json userEmu = minimal("emu");
    userEmu["displayName"] = "My Emu";
    nlohmann::json brokenUser = minimal("other");
    brokenUser["wait"] = "never";
    ProfileDocument shipped = parseProfileDocument(
        nlohmann::json{{"schemaVersion", 1}, {"profiles", {minimal("first"), shippedEmu, minimal("other")}}},
        "shipped.json", ProfileOrigin::Shipped);
    ProfileDocument user = parseProfileDocument(
        nlohmann::json{{"schemaVersion", 1}, {"profiles", {minimal("mine"), userEmu, brokenUser}}}, "user.json",
        ProfileOrigin::User);
    REQUIRE(shipped.issues.empty());
    REQUIRE(user.issues.size() == 1);

    const EmulatorProfiles merged = mergeProfileDocuments(std::move(shipped), std::move(user));
    REQUIRE(merged.profiles.size() == 4);
    CHECK(merged.profiles[0].id == "first");
    CHECK(merged.profiles[1].id == "emu"); // replaced where it was
    CHECK(merged.profiles[1].displayName == "My Emu");
    CHECK(merged.profiles[1].origin == ProfileOrigin::User);
    CHECK(merged.profiles[1].source == "user.json");
    CHECK(merged.profiles[2].id == "other"); // the user's broken one does not replace it
    CHECK(merged.profiles[2].origin == ProfileOrigin::Shipped);
    CHECK(merged.profiles[3].id == "mine"); // new ones after the shipped ones
    REQUIRE(merged.issues.size() == 1);
    CHECK(merged.issues[0].file == "user.json");
    CHECK(merged.issues[0].profileId == "other");

    REQUIRE(merged.find("mine") != nullptr);
    CHECK(merged.find("mine")->origin == ProfileOrigin::User);
    CHECK(merged.find("missing") == nullptr);
}

TEST_CASE("launcher/emulator_profile: profiles served by a system", "[launcher][emulator_profile]") {
    nlohmann::json a = minimal("a");
    a["systems"] = {{"gb", nlohmann::json::object()}, {"snes", nlohmann::json::object()}};
    nlohmann::json b = minimal("b");
    b["systems"] = {{"gb", nlohmann::json::object()}};
    const EmulatorProfiles profiles = mergeProfileDocuments(
        parseProfileDocument(nlohmann::json{{"schemaVersion", 1}, {"profiles", {a, b, minimal("c")}}}, "f.json",
                             ProfileOrigin::Shipped),
        {});
    const auto gb = profiles.serving("gb");
    REQUIRE(gb.size() == 2);
    CHECK(gb[0]->id == "a");
    CHECK(gb[1]->id == "b");
    CHECK(profiles.serving("snes").size() == 1);
    CHECK(profiles.serving("psx").empty());
}

TEST_CASE("launcher/emulator_profile: files", "[launcher][emulator_profile]") {
    rv::test::TempDir dir("rv-profiles");
    const std::filesystem::path shipped = dir.file("assets/launcher/emulators.json");
    const std::filesystem::path user = dir.file("data/config/emulators.json");
    write(shipped, R"(// shipped
{"schemaVersion": 1, "profiles": [{"id": "emu", "executable": "C:\\emu.exe", "arguments": ["{rom}"]}]})");

    SECTION("no user file: the shipped profiles, no issue") {
        const EmulatorProfiles profiles = loadEmulatorProfiles(shipped, user);
        CHECK(profiles.issues.empty());
        REQUIRE(profiles.profiles.size() == 1);
        CHECK(profiles.profiles[0].source == rv::core::pathToUtf8(shipped));
    }
    SECTION("a user file that does not parse is one issue; the shipped profiles stay") {
        write(user, R"({"schemaVersion": 1, "profiles": [ )");
        const EmulatorProfiles profiles = loadEmulatorProfiles(shipped, user);
        CHECK(profiles.profiles.size() == 1);
        REQUIRE(profiles.issues.size() == 1);
        CHECK(profiles.issues[0].error.code == ErrorCode::ParseError);
        CHECK(profiles.issues[0].file == rv::core::pathToUtf8(user));
    }
    SECTION("a user file with comments replaces and adds") {
        write(user, R"({
            // mine
            "schemaVersion": 1,
            "profiles": [
                {"id": "emu", "displayName": "Mine", "executable": "D:\\emu.exe", "arguments": []}, // replaces
                {"id": "new", "executable": "D:\\new.exe", "arguments": ["{rom}"]}
            ]
        })");
        const EmulatorProfiles profiles = loadEmulatorProfiles(shipped, user);
        CHECK(profiles.issues.empty());
        REQUIRE(profiles.profiles.size() == 2);
        CHECK(profiles.profiles[0].displayName == "Mine");
        CHECK(profiles.profiles[1].id == "new");
    }
    SECTION("a missing shipped file is an issue") {
        const EmulatorProfiles profiles = loadEmulatorProfiles(dir.file("nowhere.json"), user);
        CHECK(profiles.profiles.empty());
        REQUIRE(profiles.issues.size() == 1);
        CHECK(profiles.issues[0].error.code == ErrorCode::NotFound);
    }
    SECTION("through the application's paths") {
        rv::core::PathOptions options;
        options.assetRootOverride = dir.file("assets");
        options.userDataOverride = dir.file("data");
        rv::core::PathEnvironment environment;
        environment.executableDir = dir.path();
        const auto paths = rv::core::Paths::resolve(options, environment);
        REQUIRE(paths.has_value());
        write(user, R"({"schemaVersion": 1, "profiles": [{"id": "new", "executable": "D:\\n.exe", "arguments": []}]})");
        const EmulatorProfiles profiles = loadEmulatorProfiles(*paths);
        CHECK(profiles.issues.empty());
        CHECK(profiles.profiles.size() == 2);
    }
}

TEST_CASE("launcher/emulator_profile: the shipped defaults", "[launcher][emulator_profile]") {
    const ProfileDocument document = loadProfileDocument(
        std::filesystem::path(RV_TEST_ASSET_DIR) / "launcher" / "emulators.json", ProfileOrigin::Shipped);
    for (const auto& issue : document.issues) {
        UNSCOPED_INFO(issue.toString());
    }
    REQUIRE(document.issues.empty());
    const auto retroarch = std::find_if(document.profiles.begin(), document.profiles.end(),
                                        [](const EmulatorProfile& profile) { return profile.id == "retroarch"; });
    REQUIRE(retroarch != document.profiles.end());
    CHECK(retroarch->executable.appPathsName == "retroarch.exe");
    CHECK(retroarch->arguments == std::vector<std::string>{"-L", "{var:core}", "{rom}"});
    CHECK(retroarch->variables.at("cores") == R"({emulator_dir}\cores)");
    CHECK(retroarch->requiredFiles == std::vector<std::string>{"{var:core}"});
    CHECK_FALSE(retroarch->verified);
    CHECK(retroarch->serves("gb"));
    CHECK(retroarch->serves("snes"));
    CHECK(retroarch->serves("psx"));
    for (const EmulatorProfile& profile : document.profiles) {
        CHECK_FALSE(profile.verified);
        // No machine path (specification §11.1): App Paths, the home folder or an environment variable.
        for (const std::string& path : profile.executable.paths) {
            INFO(profile.id << ": " << path);
            CHECK((path.starts_with("~\\") || path.starts_with("{env:")));
        }
        for (const auto& [system, settings] : profile.systems) {
            INFO(profile.id << ", " << system);
            const auto core = settings.variables.find("core");
            REQUIRE(core != settings.variables.end());
            CHECK(core->second.starts_with("{var:cores}\\"));
            CHECK(core->second.ends_with("_libretro.dll"));
        }
    }
}

TEST_CASE("launcher/emulator_profile: executables are located through the probe", "[launcher][emulator_profile]") {
    rv::test::FakeSystemProbe probe;
    probe.setHomeFolder(std::filesystem::path(LR"(C:\Users\Player)"));
    const auto environment = environmentOf({{"PROGRAMFILES", R"(C:\Program Files)"}});

    SECTION("App Paths first, HKCU before HKLM") {
        probe.setRegistryString(RegistryRoot::LocalMachine, std::string(kAppPaths) + "retroarch.exe", "",
                                R"("E:\Programs\RetroArch\retroarch.exe")");
        probe.addFile(LR"(E:\Programs\RetroArch\retroarch.exe)");
        probe.addFile(LR"(C:\Users\Player\AppData\Roaming\RetroArch\retroarch.exe)");
        const ExecutableSpec spec{"retroarch.exe", {R"(~\AppData\Roaming\RetroArch\retroarch.exe)"}};
        const auto located = locateExecutable(spec, probe, environment);
        REQUIRE(located.has_value());
        CHECK(located->path == std::filesystem::path(LR"(E:\Programs\RetroArch\retroarch.exe)"));
        CHECK(located->rule == LocationRule::AppPaths);
        CHECK(located->entry == "retroarch.exe");

        probe.setRegistryString(RegistryRoot::CurrentUser, std::string(kAppPaths) + "retroarch.exe", "",
                                R"(D:\RA\retroarch.exe)");
        probe.addFile(LR"(D:\RA\retroarch.exe)");
        CHECK(locateExecutable(spec, probe, environment)->path == std::filesystem::path(LR"(D:\RA\retroarch.exe)"));
    }
    SECTION("a registered file that is gone: the paths, then") {
        probe.setRegistryString(RegistryRoot::LocalMachine, std::string(kAppPaths) + "retroarch.exe", "",
                                R"(K:\Gone\retroarch.exe)");
        probe.addFile(LR"(C:\Users\Player\AppData\Roaming\RetroArch\retroarch.exe)");
        const ExecutableSpec spec{"retroarch.exe", {R"(~\AppData\Roaming\RetroArch\retroarch.exe)"}};
        const auto located = locateExecutable(spec, probe, environment);
        REQUIRE(located.has_value());
        CHECK(located->path == std::filesystem::path(LR"(C:\Users\Player\AppData\Roaming\RetroArch\retroarch.exe)"));
        CHECK(located->rule == LocationRule::Path);
        CHECK(located->entry == R"(~\AppData\Roaming\RetroArch\retroarch.exe)");
    }
    SECTION("environment variables, '..' and wildcards in the last element") {
        probe.addFile(LR"(C:\Program Files\Emu\emu-qt-x64-b.exe)");
        probe.addFile(LR"(C:\Program Files\Emu\emu-qt-x64-a.exe)");
        probe.addFile(LR"(C:\Program Files\Emu\other.exe)");
        const ExecutableSpec spec{{}, {R"({env:PROGRAMFILES}\Emu\..\Emu\EMU-QT*.exe)"}};
        const auto located = locateExecutable(spec, probe, environment);
        REQUIRE(located.has_value());
        CHECK(located->path == std::filesystem::path(LR"(C:\Program Files\Emu\emu-qt-x64-a.exe)")); // sorted first
        CHECK(located->rule == LocationRule::Path);
    }
    SECTION("nothing found: every attempt is named") {
        probe.setHomeFolder(std::nullopt);
        const ExecutableSpec spec{"emu.exe",
                                  {R"(~\Emu\emu.exe)", R"({env:LOCALAPPDATA}\Emu\emu.exe)", R"(C:\Emu\emu.exe)",
                                   R"(C:\Emu\emu*.exe)"}};
        const auto located = locateExecutable(spec, probe, environment);
        REQUIRE_FALSE(located.has_value());
        CHECK(located.error().code == ErrorCode::NotFound);
        const std::string& message = located.error().message;
        CHECK(message.find("App Paths \"emu.exe\" is not registered") != std::string::npos);
        CHECK(message.find("home folder is not known") != std::string::npos);
        CHECK(message.find("{env:LOCALAPPDATA} is not set") != std::string::npos);
        CHECK(message.find(R"(C:\Emu\emu.exe does not exist)") != std::string::npos);
        CHECK(message.find(R"(no file matches C:\Emu\emu*.exe)") != std::string::npos);
    }
    SECTION("a registered relative path is not used") {
        probe.setRegistryString(RegistryRoot::CurrentUser, std::string(kAppPaths) + "emu.exe", "", "emu.exe");
        const auto located = locateExecutable(ExecutableSpec{"emu.exe", {}}, probe, environment);
        REQUIRE_FALSE(located.has_value());
        CHECK(located.error().message.find("relative path") != std::string::npos);
    }
    SECTION("an empty spec") {
        CHECK_FALSE(locateExecutable(ExecutableSpec{}, probe, environment).has_value());
    }
}
