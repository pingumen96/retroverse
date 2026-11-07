// I11 step 13 (ADR-0020 §3, §4): ES-DE's "open the game's file" launched for real through retroverse-cli, on the
// shortcuts fixture: a shortcut starts its program (the build's fake emulator), a game that leaves the launcher's tree is
// followed to its end, and an automated run never activates a protocol.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include "support/child_process.hpp"
#include "support/fixture.hpp"
#include "support/shortcut_file.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(fs::path(path).make_preferred());
}

struct CliRun {
    int exitCode = -1;
    json result;
    std::string standardError;
};

/// The shortcuts fixture, a data directory beside it, and retroverse-cli runs in the background with only the build's
/// fake emulator allowed.
struct ShortcutFlow {
    rv::test::TempDir dir{"rv-cli-shortcuts"};
    rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/shortcuts").value();
    fs::path data = dir.file("data");
    fs::path fake = fs::absolute(rv::test::testProgramsDir() / "rv_fake_emulator.exe");
    int runs = 0;

    CliRun cli(std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(data)});
        arguments.push_back("--json");
        rv::test::ChildRequest request;
        request.executable = rv::test::testProgramsDir() / "retroverse-cli.exe";
        request.arguments = arguments;
        request.workingDirectory = dir.path();
        ++runs;
        request.stdoutFile = dir.file(fmt::format("run{:02}.out", runs));
        request.stderrFile = dir.file(fmt::format("run{:02}.err", runs));
        request.environment = {{"RV_LAUNCH_BACKGROUND", std::string("1")}, {"RV_LAUNCH_ALLOWLIST", utf8(fake)}};
        request.timeout = std::chrono::minutes(2);
        const auto outcome = rv::test::runChild(request);
        REQUIRE(outcome.has_value());
        REQUIRE_FALSE(outcome->timedOut);
        CliRun run;
        run.exitCode = outcome->exitCode;
        run.standardError = outcome->standardError;
        if (!outcome->standardOutput.empty()) {
            run.result = json::parse(outcome->standardOutput, nullptr, false);
            INFO(outcome->standardOutput);
            REQUIRE_FALSE(run.result.is_discarded());
        }
        return run;
    }

    json ok(std::vector<std::string> arguments) {
        const std::string name = arguments.front();
        const CliRun run = cli(std::move(arguments));
        INFO(name << ": " << run.standardError << run.result.dump(2));
        REQUIRE(run.exitCode == 0);
        return run.result;
    }

    std::string gameId(const std::string& search) {
        const json found = ok({"search", search});
        REQUIRE(found.at("games").size() == 1);
        return found.at("games").at(0).at("id").get<std::string>();
    }

    void shortcut(const std::string& name, const std::string& arguments, const fs::path& folder = {}) {
        const rv::Status written = rv::test::writeShortcut(fixture.path("roms/windows/" + name), fake, arguments, folder);
        INFO((written ? std::string() : written.error().toString()));
        REQUIRE(written);
    }
};

} // namespace

TEST_CASE("cli/launch: \"open the game's file\" starts a shortcut's program, follows a game outside its tree, never "
          "activates a protocol (I11 step 13)",
          "[cli][launch][shortcut]") {
    ShortcutFlow flow;
    const fs::path argsFile = flow.dir.file("kettle_args.json");
    const fs::path cwdFile = flow.dir.file("kettle_cwd.json");
    const fs::path folder = flow.dir.file("Kettle Kit");
    fs::create_directories(folder);
    flow.shortcut("Kettle Kit.lnk",
                  fmt::format("--frames 20 --min-window-ms 2500 --print-args \"{}\" --print-cwd \"{}\" -- \"two words\" last", utf8(argsFile),
                              utf8(cwdFile)),
                  folder);
    // A launcher that starts the game outside its tree and exits at once; the game runs 2.5 s, exits with 4.
    flow.shortcut("Ember Gate.lnk",
                  "--spawn-child-and-exit --child-breakaway --frames 30 --min-window-ms 2500 --exit-code 4");
    flow.ok({"library", "create", "--home", utf8(flow.fixture.root()), "--install",
             utf8(flow.fixture.path("Programs/ES-DE"))});
    flow.ok({"import"});
    // The invented store's scheme, listed by the user's emulators.json.
    REQUIRE(rv::core::writeTextFileAtomic(
                flow.data / "config" / "emulators.json",
                R"({"schemaVersion": 2, "profiles": [],
                    "protocols": [{"scheme": "lanternstore", "launcherProcesses": ["lantern*.exe"]}]})")
                .has_value());

    SECTION("a shortcut: its program, its arguments, its working folder; cmd.exe never runs") {
        const std::string kettle = flow.gameId("kettle");
        const json dry = flow.ok({"launch", "--dry-run", "--game", kettle});
        INFO(dry.dump(2));
        CHECK(dry.at("readiness") == "ready");
        CHECK(dry.dump().find("cmd.exe") == std::string::npos);
        const json launched = flow.ok({"launch", "--game", kettle, "--wait"});
        INFO(launched.dump(2));
        CHECK(launched.at("session").at("outcome") == "returned");
        CHECK(launched.at("session").at("exitCode") == 0);
        const auto args = rv::core::readJsonFile(argsFile);
        REQUIRE(args.has_value());
        CHECK((*args).at("payload") == json::array({"two words", "last"}));
        const auto cwd = rv::core::readJsonFile(cwdFile);
        REQUIRE(cwd.has_value());
        CHECK(fs::equivalent(rv::core::pathFromUtf8((*cwd).at("cwd").get<std::string>()), folder));
    }
    SECTION("%RUNINBACKGROUND%: the game a launcher started outside its tree, followed to its end") {
        flow.ok({"assign", "windows", "--command", "Shortcut or script (Keep ES-DE running)"});
        const std::string ember = flow.gameId("ember");
        const json launched = flow.ok({"launch", "--game", ember, "--wait"});
        INFO(launched.dump(2));
        const json& session = launched.at("session");
        if (session.at("exitCode") == 3) {
            SKIP("a job this test runs in forbids breaking away (the launcher could not start its game)");
        }
        CHECK(session.at("exitCode") == 4);
        CHECK(session.at("durationMs").get<std::int64_t>() >= 2500);
        CHECK_FALSE(session.at("quickExit").get<bool>());
    }
    SECTION("an Internet Shortcut: activated by a protocol the user lists, never in an automated run") {
        const std::string lantern = flow.gameId("lantern");
        const json dry = flow.ok({"launch", "--dry-run", "--game", lantern});
        INFO(dry.dump(2));
        CHECK(dry.at("readiness") == "ready");
        CHECK(dry.dump().find("lanternstore://launch/400") != std::string::npos);
        CHECK(dry.dump().find("FollowWindow") != std::string::npos);
        const CliRun refused = flow.cli({"launch", "--game", lantern, "--wait"});
        INFO(refused.standardError << refused.result.dump(2));
        CHECK(refused.exitCode != 0);
        CHECK((refused.result.dump() + refused.standardError).find("protocol activation never runs") !=
              std::string::npos);
    }
}
