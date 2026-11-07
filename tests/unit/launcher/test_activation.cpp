// I11 step 13 (ADR-0020 §3, §4): protocol activations, the protocols emulators.json lists, FollowWindow's session.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/launch_request.hpp>

#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

using rv::launcher::EmulatorProfiles;
using rv::launcher::LaunchRequest;
using rv::launcher::ProfileDocument;
using rv::launcher::ProfileOrigin;
using rv::launcher::ProtocolRule;
using rv::launcher::WaitStrategy;

namespace {

LaunchRequest activation(std::string url) {
    LaunchRequest request;
    request.activationUrl = std::move(url);
    request.waitStrategy = WaitStrategy::FollowWindow;
    return request;
}

ProfileDocument parse(const nlohmann::json& document, ProfileOrigin origin = ProfileOrigin::User) {
    return rv::launcher::parseProfileDocument(document, "emulators.json", origin);
}

nlohmann::json withProtocols(nlohmann::json protocols, int schema = 2) {
    return nlohmann::json{{"schemaVersion", schema}, {"profiles", nlohmann::json::array()}, {"protocols", protocols}};
}

} // namespace

TEST_CASE("launcher/activation: the schemes and URLs accepted", "[launcher][activation]") {
    CHECK(rv::launcher::activationScheme("steam://rungameid/400") == std::optional<std::string>("steam"));
    CHECK(rv::launcher::activationScheme("Com.EpicGames.Launcher://apps/x") ==
          std::optional<std::string>("com.epicgames.launcher"));
    CHECK_FALSE(rv::launcher::activationScheme("://x"));
    CHECK_FALSE(rv::launcher::activationScheme("1steam://x"));
    CHECK_FALSE(rv::launcher::activationScheme("ste am://x"));
    CHECK_FALSE(rv::launcher::activationScheme("C:\\Games\\game.exe") == std::optional<std::string>("steam"));

    CHECK(rv::launcher::checkActivationUrl("steam://rungameid/400"));
    CHECK(rv::launcher::checkActivationUrl("goggalaxy://openGameView/1207658924"));
    for (const char* refused :
         {"", "steam:", "steam://run game", "steam://run\"x", "steam://\x01", "http://example.com", "HTTPS://x",
          "file:///C:/Windows/notepad.exe", "shell:startup", "search-ms:query=x", "ms-settings:display",
          "microsoft-edge:https://x", "javascript:alert(1)", "mailto:a@b"}) {
        INFO(refused);
        const rv::Status status = rv::launcher::checkActivationUrl(refused);
        REQUIRE_FALSE(status);
        CHECK(status.error().code == rv::ErrorCode::ValidationFailed);
    }
    CHECK_FALSE(rv::launcher::checkActivationUrl("steam://" + std::string(rv::launcher::kMaxActivationUrlLength, 'x')));
    CHECK(rv::launcher::parseWaitStrategy("follow") == WaitStrategy::FollowWindow);
    CHECK(rv::launcher::toString(WaitStrategy::FollowWindow) == "FollowWindow");
    CHECK(rv::launcher::effectiveStrategy(WaitStrategy::FollowWindow, false) == WaitStrategy::FollowWindow);
}

TEST_CASE("launcher/activation: a request that activates names nothing else", "[launcher][activation]") {
    rv::test::TempDir dir("rv-activation");
    CHECK(rv::launcher::validateLaunchRequest(activation("steam://rungameid/400")));
    CHECK(rv::launcher::commandLineOf(activation("steam://rungameid/400")).value() == "steam://rungameid/400");
    CHECK(rv::launcher::workingDirectoryOf(activation("steam://rungameid/400")).empty());

    LaunchRequest withProgram = activation("steam://rungameid/400");
    withProgram.executable = rv::test::writeProgramStub(dir.file("game.exe"));
    REQUIRE_FALSE(rv::launcher::validateLaunchRequest(withProgram));
    CHECK(rv::launcher::validateLaunchRequest(withProgram).error().context == "executable");
    LaunchRequest withArguments = activation("steam://rungameid/400");
    withArguments.arguments = {"-x"};
    CHECK_FALSE(rv::launcher::validateLaunchRequest(withArguments));
    CHECK_FALSE(rv::launcher::validateLaunchRequest(activation("https://example.com")));
}

TEST_CASE("launcher/activation: an automated run never activates a protocol", "[launcher][activation]") {
    const LaunchRequest request = activation("steam://rungameid/400");
    const rv::launcher::GuardVerdict refused = rv::launcher::LaunchGuard::fromAllowList("").check(request);
    CHECK(refused.rule == rv::launcher::GuardRule::Activation);
    CHECK_FALSE(refused.allowed());
    CHECK(refused.message.find("steam://rungameid/400") != std::string::npos);
    CHECK(rv::launcher::LaunchGuard{}.check(request).allowed());
    CHECK(rv::launcher::toString(rv::launcher::GuardRule::Activation) == "activation");
}

TEST_CASE("launcher/activation: emulators.json's protocols", "[launcher][activation]") {
    SECTION("a rule, its scheme in lower case") {
        const ProfileDocument document =
            parse(withProtocols({{{"scheme", "Steam"}, {"launcherProcesses", {"steam.exe", "steamwebhelper*.exe"}}}}));
        REQUIRE(document.issues.empty());
        REQUIRE(document.protocols.size() == 1);
        CHECK(document.protocols[0].scheme == "steam");
        CHECK(document.protocols[0].launcherProcesses == std::vector<std::string>{"steam.exe", "steamwebhelper*.exe"});
        CHECK(document.protocols[0].origin == ProfileOrigin::User);
    }
    SECTION("each malformed rule left out and named") {
        const ProfileDocument document = parse(withProtocols({{{"scheme", "http"}},
                                                              {{"scheme", "1x"}},
                                                              {{"launcherProcesses", {"a.exe"}}},
                                                              {{"scheme", "uplay"}, {"launcherProcesses", {"a\\b.exe"}}},
                                                              {{"scheme", "uplay"}, {"other", 1}},
                                                              {{"scheme", "steam"}},
                                                              {{"scheme", "STEAM"}}}));
        REQUIRE(document.protocols.size() == 1);
        CHECK(document.protocols[0].scheme == "steam");
        CHECK(document.issues.size() == 6);
    }
    SECTION("protocols only in schema 2") {
        const ProfileDocument document = parse(withProtocols({{{"scheme", "steam"}}}, 1));
        CHECK(document.protocols.empty());
        REQUIRE(document.issues.size() == 1);
        CHECK(document.issues[0].toString().find("needs schemaVersion 2") != std::string::npos);
    }
    SECTION("the user's rule replaces the shipped one for its scheme") {
        ProfileDocument shipped = parse(withProtocols({{{"scheme", "steam"}, {"launcherProcesses", {"steam.exe"}}},
                                                       {{"scheme", "uplay"}}}),
                                        ProfileOrigin::Shipped);
        ProfileDocument user = parse(withProtocols({{{"scheme", "steam"}, {"launcherProcesses", {"other.exe"}}},
                                                    {{"scheme", "itch"}}}));
        const EmulatorProfiles merged = rv::launcher::mergeProfileDocuments(std::move(shipped), std::move(user));
        REQUIRE(merged.protocols.size() == 3);
        REQUIRE(merged.protocolFor("STEAM") != nullptr);
        CHECK(merged.protocolFor("steam")->launcherProcesses == std::vector<std::string>{"other.exe"});
        CHECK(merged.protocolFor("steam")->origin == ProfileOrigin::User);
        CHECK(merged.protocolFor("uplay") != nullptr);
        CHECK(merged.protocolFor("itch") != nullptr);
        CHECK(merged.protocolFor("https") == nullptr);
    }
    SECTION("the shipped protocols") {
        const ProfileDocument document = rv::launcher::loadProfileDocument(
            std::filesystem::path(RV_TEST_ASSET_DIR) / "launcher" / "emulators.json", ProfileOrigin::Shipped);
        CHECK(document.issues.empty());
        const EmulatorProfiles profiles = rv::launcher::mergeProfileDocuments(document, ProfileDocument{});
        for (const char* scheme : {"steam", "com.epicgames.launcher", "goggalaxy", "uplay", "origin2"}) {
            INFO(scheme);
            const ProtocolRule* rule = profiles.protocolFor(scheme);
            REQUIRE(rule != nullptr);
            CHECK_FALSE(rule->launcherProcesses.empty());
        }
        REQUIRE(profiles.protocolFor("steam") != nullptr);
        CHECK(profiles.protocolFor("steam")->launcherProcesses.front() == "steam.exe");
    }
}
