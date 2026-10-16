#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>

#include "support/temp_dir.hpp"

using rv::cli::Command;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

/// The commands of I2 in the order of --help, with the step of docs/roadmap.md that implements each.
constexpr std::array<std::pair<std::string_view, int>, 14> kPlannedCommands{{
    {"discover", 2},
    {"library", 2},
    {"db", 4},
    {"import", 7},
    {"systems", 9},
    {"games", 9},
    {"search", 9},
    {"show", 9},
    {"media", 8},
    {"profiles", 10},
    {"launch", 10},
    {"identities", 11},
    {"assign", 12},
    {"sessions", 12},
}};

/// Directories of a test run: the source tree's assets and no %LOCALAPPDATA%, so a run never reaches the real user
/// data (tests pass --data-dir).
rv::core::PathEnvironment testEnvironment(const std::filesystem::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger on stdout afterwards.
class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

RunResult runCli(const std::vector<std::string>& arguments, const rv::core::PathEnvironment& environment) {
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code = rv::cli::run(arguments, out, err, environment);
    return RunResult{code, out.text(), err.text()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// What a command prints until its step implements it.
std::string stubMessage(const Command& command) {
    return fmt::format("retroverse-cli {}: not implemented yet (I2 step {})\n", command.name, command.step);
}

/// An option no command declares: a usage error for every command, stub or implemented, so the tests that pass it to
/// every row of the table stay as they are when a step implements a command.
constexpr std::string_view kUnknownOption = "--no-such-option";

} // namespace

TEST_CASE("cli/command: the table lists the 14 commands of I2 in order, each with the step that implements it",
          "[cli]") {
    const std::span<const Command> table = rv::cli::commands();
    REQUIRE(table.size() == kPlannedCommands.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
        INFO("row " << i);
        CHECK(table[i].name == kPlannedCommands[i].first);
        CHECK(table[i].step == kPlannedCommands[i].second);
        CHECK_FALSE(table[i].summary.empty());
        CHECK(table[i].summary.find('\n') == std::string_view::npos);
        CHECK(table[i].run != nullptr);
        CHECK(rv::cli::findCommand(table[i].name) == &table[i]);
    }
    // Names match exactly.
    CHECK(rv::cli::findCommand("Discover") == nullptr);
    CHECK(rv::cli::findCommand("") == nullptr);
    CHECK(rv::cli::findCommand("--help") == nullptr);

    // --help lists every command with its summary, the global options and the exit codes.
    const std::string help = rv::cli::helpText();
    for (const Command& command : table) {
        CHECK(contains(help, fmt::format("  {}", command.name)));
        CHECK(contains(help, command.summary));
    }
    for (const std::string_view option : {"--data-dir", "--portable", "--asset-root", "--log-level", "--version"}) {
        CHECK(contains(help, option));
    }
    CHECK(contains(help, "Exit codes: 0 ok; 1 a check found problems; 2 usage or initialization error; 3 runtime"));
}

TEST_CASE("cli/command: a stub names its command and step on stderr and exits 2", "[cli]") {
    const rv::test::TempDir dir("rv-cli-stub");
    rv::core::PathOptions pathOptions;
    pathOptions.userDataOverride = dir.path();
    const auto paths = rv::core::Paths::resolve(pathOptions, testEnvironment(dir.path()));
    REQUIRE(paths.has_value());
    const rv::cli::GlobalOptions global;
    const std::vector<std::string> arguments{"--help"}; // a stub ignores its arguments, --help included

    // A row of no table: the message stays tested once every command of I2 is implemented.
    const Command stub{"frobnicate", "Not a command", 99, rv::cli::notImplemented};
    Output out = Output::memory();
    Output err = Output::memory();
    rv::cli::CommandContext context{stub, global, arguments, *paths, out, err};
    CHECK(stub.run(context) == ExitCode::UsageError);
    CHECK(out.text().empty());
    CHECK(err.text() == "retroverse-cli frobnicate: not implemented yet (I2 step 99)\n");
}

TEST_CASE("cli/command: every command exits 2 on an unknown option with nothing on stdout; a stub names its step",
          "[cli]") {
    const rv::test::TempDir dir("rv-cli-stubs");
    rv::core::PathOptions pathOptions;
    pathOptions.userDataOverride = dir.path();
    const auto paths = rv::core::Paths::resolve(pathOptions, testEnvironment(dir.path()));
    REQUIRE(paths.has_value());
    const rv::cli::GlobalOptions global;
    const std::vector<std::string> arguments{std::string(kUnknownOption)};

    for (const Command& command : rv::cli::commands()) {
        INFO(command.name);
        Output out = Output::memory();
        Output err = Output::memory();
        rv::cli::CommandContext context{command, global, arguments, *paths, out, err};
        CHECK(command.run(context) == ExitCode::UsageError);
        CHECK(out.text().empty());
        CHECK_FALSE(err.text().empty());
        // Until its step implements it, the command answers as a stub, whatever its arguments.
        if (contains(err.text(), "not implemented yet")) {
            CHECK(err.text() == stubMessage(command));
        }
    }
}

TEST_CASE("cli/run: --version and --help print to stdout and exit 0; usage errors exit 2 on stderr", "[cli]") {
    const rv::test::TempDir dir("rv-cli-usage");
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());

    const RunResult version = runCli({"--version"}, environment);
    CHECK(version.code == ExitCode::Ok);
    CHECK(version.out == rv::core::versionBanner() + "\n");
    CHECK(version.err.empty());

    for (const std::string_view flag : {"--help", "-h"}) {
        const RunResult help = runCli({std::string(flag)}, environment);
        CHECK(help.code == ExitCode::Ok);
        CHECK(help.out == rv::cli::helpText());
        CHECK(help.err.empty());
    }
    // --help wins over a command and its arguments.
    CHECK(runCli({"--help", "discover", "--anything"}, environment).code == ExitCode::Ok);

    // No command: the command table on stderr.
    const RunResult none = runCli({}, environment);
    CHECK(none.code == ExitCode::UsageError);
    CHECK(none.out.empty());
    CHECK(none.err == rv::cli::helpText());
    CHECK(runCli({"--log-level", "debug"}, environment).code == ExitCode::UsageError);

    const RunResult unknown = runCli({"frobnicate", "--now"}, environment);
    CHECK(unknown.code == ExitCode::UsageError);
    CHECK(unknown.out.empty());
    CHECK(contains(unknown.err, "retroverse-cli: unknown command 'frobnicate'"));

    // Invalid global options name the option.
    const std::vector<std::pair<std::vector<std::string>, std::string_view>> invalid{
        {{"--bogus", "discover"}, "--bogus"},
        {{"--data-dir"}, "--data-dir"},
        {{"--data-dir=", "discover"}, "--data-dir"},
        {{"--data-dir", "D:/x", "--portable", "discover"}, "--portable"},
        {{"--log-level", "loud", "discover"}, "--log-level=loud"},
        {{"--portable=yes", "discover"}, "--portable=yes"},
    };
    for (const auto& [arguments, named] : invalid) {
        INFO(fmt::format("{}", fmt::join(arguments, " ")));
        const RunResult result = runCli(arguments, environment);
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
        CHECK(contains(result.err, named));
    }
}

TEST_CASE("cli/run: global options end at the command, whose arguments follow untouched", "[cli]") {
    const std::vector<std::string> arguments{"--data-dir=D:/data", "--asset-root", "D:/assets", "--log-level",
                                             "WARN",               "search",       "--output",  "r.txt",
                                             "--data-dir",         "mario"};
    const auto invocation = rv::cli::parseInvocation(arguments);
    REQUIRE(invocation.has_value());
    CHECK(invocation->global.dataDir == std::filesystem::path("D:/data"));
    CHECK(invocation->global.assetRoot == std::filesystem::path("D:/assets"));
    CHECK(invocation->global.logLevel == spdlog::level::warn);
    CHECK_FALSE(invocation->global.portable);
    CHECK_FALSE(invocation->global.showHelp);
    CHECK_FALSE(invocation->global.showVersion);
    CHECK(invocation->command == "search");
    CHECK(invocation->arguments == std::vector<std::string>{"--output", "r.txt", "--data-dir", "mario"});

    const auto portable = rv::cli::parseInvocation(std::vector<std::string>{"--portable", "db", "check"});
    REQUIRE(portable.has_value());
    CHECK(portable->global.portable);
    CHECK_FALSE(portable->global.dataDir.has_value());
    CHECK(portable->global.logLevel == spdlog::level::info);
    CHECK(portable->command == "db");
    CHECK(portable->arguments == std::vector<std::string>{"check"});

    const auto empty = rv::cli::parseInvocation(std::vector<std::string>{});
    REQUIRE(empty.has_value());
    CHECK(empty->command.empty());
    CHECK(empty->arguments.empty());
}

TEST_CASE("cli/run: a command runs with its directories resolved and logs to stderr and its log file only", "[cli]") {
    const rv::test::TempDir dir("rv-cli-run");
    const LoggingReset loggingReset;
    const std::filesystem::path data = dir.file("data");

    // Each command through the dispatcher: the same exit code as when called directly, nothing on stdout, and a stub's
    // message alone on stderr (the console sink shows warnings and errors only at the default --log-level).
    for (const Command& command : rv::cli::commands()) {
        INFO(command.name);
        const RunResult result =
            runCli({"--data-dir", rv::core::pathToUtf8(data), std::string(command.name), std::string(kUnknownOption)},
                   testEnvironment(dir.path()));
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
        CHECK_FALSE(result.err.empty());
        if (contains(result.err, "not implemented yet")) {
            CHECK(result.err == stubMessage(command));
        }
    }

    // The log file is <user data>/logs/retroverse-cli.log, closed when the run ends.
    const auto log = rv::core::readTextFile(data / "logs" / "retroverse-cli.log");
    REQUIRE(log.has_value());
    CHECK(contains(*log, "retroverse-cli discover"));
    CHECK(contains(*log, "retroverse-cli sessions: exit code 2"));
    CHECK(rv::core::activeLogFile().empty());
}

TEST_CASE("cli/run: directories that cannot be resolved exit 2", "[cli]") {
    const rv::test::TempDir dir("rv-cli-paths");
    const LoggingReset loggingReset;
    const RunResult result = runCli({"--asset-root", rv::core::pathToUtf8(dir.file("missing")), "--data-dir",
                                     rv::core::pathToUtf8(dir.file("data")), "discover"},
                                    testEnvironment(dir.path()));
    CHECK(result.code == ExitCode::UsageError);
    CHECK(result.out.empty());
    CHECK(contains(result.err, "retroverse-cli: cannot locate its directories"));
    CHECK(contains(result.err, "--asset-root"));
    CHECK_FALSE(std::filesystem::exists(dir.file("data")));
}
