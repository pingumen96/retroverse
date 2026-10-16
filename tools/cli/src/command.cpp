#include <rv/cli/command.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>

#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kProgram = "retroverse-cli";
constexpr std::string_view kLogFileName = "retroverse-cli.log";

/// The command table: the order of --help, the step of docs/roadmap.md that implements each command. The rows never
/// change when a command is implemented: its cmd_<name>.cpp does.
constexpr std::array<Command, 14> kCommands{{
    {"discover", "Find the ES-DE installation: its home, install, ROM and media folders", 2, runDiscover},
    {"library", "Show the library: its id, the ES-DE folders it reads and their overrides (library.json)", 2,
     runLibrary},
    {"db", "Maintain the catalog database: migrate, check, rebuild-index", 4, runDb},
    {"import", "Import the ES-DE collection into the catalog (--dry-run: what would change)", 7, runImport},
    {"systems", "List the systems of the catalog with their game counts", 9, runSystems},
    {"games", "List games, filtered and sorted, a page at a time", 9, runGames},
    {"search", "Search games by title, developer, publisher, genre, year or system", 9, runSearch},
    {"show", "Show a game: metadata, files, media and launch readiness", 9, runShow},
    {"media", "Show the media files of a game, or of a ROM path of a system", 8, runMedia},
    {"profiles", "List the emulator profiles and the commands of each system with their readiness", 10, runProfiles},
    {"launch", "Launch a game in its emulator (--dry-run: the command line it would run)", 10, runLaunch},
    {"identities", "List identity changes: moved files, aliases and rows to review", 11, runIdentities},
    {"assign", "Choose the emulator command or profile of a system or a game", 12, runAssign},
    {"sessions", "List the play sessions", 12, runSessions},
}};

/// Global options that take a value (the next argument, unless given as --name=value).
constexpr std::array<std::string_view, 3> kValueOptions{"--data-dir", "--asset-root", "--log-level"};

core::ArgParser globalParser() {
    core::ArgParser parser{std::string(kProgram)};
    parser.flag("version", "Print the version and exit.")
        .option("data-dir", "dir", "User data directory: configuration, database and logs.")
        .flag("portable", "Keep user data next to the executable (RetroverseData).")
        .option("asset-root", "dir", "Asset directory (overrides RV_ASSET_ROOT and the defaults).")
        .option("log-level", "level", "One of trace, debug, info, warn, error.", "info");
    return parser;
}

Expected<spdlog::level::level_enum> logLevel(const core::ParsedArgs& args) {
    static constexpr std::array<std::pair<std::string_view, spdlog::level::level_enum>, 5> kLevels{{
        {"trace", spdlog::level::trace},
        {"debug", spdlog::level::debug},
        {"info", spdlog::level::info},
        {"warn", spdlog::level::warn},
        {"error", spdlog::level::err},
    }};
    const std::string text = args.value("log-level").value_or("info");
    for (const auto& [name, level] : kLevels) {
        if (core::equalsIgnoreCaseAscii(text, name)) {
            return level;
        }
    }
    return makeError(ErrorCode::InvalidArgument, "expected trace, debug, info, warn or error", "--log-level=" + text);
}

/// The value of a path option, if it was given; an empty value is an error.
Expected<std::optional<std::filesystem::path>> pathOption(const core::ParsedArgs& args, std::string_view name) {
    if (!args.has(name)) {
        return std::optional<std::filesystem::path>{};
    }
    const std::string text = args.value(name).value_or("");
    if (text.empty()) {
        return makeError(ErrorCode::InvalidArgument, "value must not be empty", "--" + std::string(name));
    }
    try {
        return std::optional<std::filesystem::path>(core::pathFromUtf8(text));
    } catch (const std::exception&) {
        // pathFromUtf8 throws on malformed UTF-8.
        return makeError(ErrorCode::InvalidArgument, "value is not valid UTF-8", "--" + std::string(name));
    }
}

/// Logging of a run: stderr only (never stdout), warnings and errors there unless --log-level asks for debug or trace
/// output; the log file, once the directories are known, at --log-level.
core::LogConfig logConfig(const GlobalOptions& global) {
    core::LogConfig config;
    config.fileName = std::string(kLogFileName);
    config.console = true;
    config.consoleStream = core::ConsoleStream::Stderr;
    config.level = global.logLevel;
    if (global.logLevel > spdlog::level::debug) {
        config.consoleLevel = std::max(global.logLevel, spdlog::level::warn);
    }
    return config;
}

ExitCode runCommand(const Command& command, const Invocation& invocation, Output& out, Output& err,
                    const core::PathEnvironment& environment) {
    // Until the directories are known, logging goes to stderr only: nothing a run logs ever reaches stdout.
    core::LogConfig config = logConfig(invocation.global);
    core::initLogging(config);

    core::PathOptions pathOptions;
    pathOptions.assetRootOverride = invocation.global.assetRoot;
    pathOptions.userDataOverride = invocation.global.dataDir;
    pathOptions.portable = invocation.global.portable;
    const auto paths = core::Paths::resolve(pathOptions, environment);
    if (!paths) {
        err.writeLine(fmt::format("{}: cannot locate its directories: {}", kProgram, paths.error().toString()));
        core::shutdownLogging();
        return ExitCode::UsageError;
    }
    config.directory = paths->logsDir();
    core::initLogging(config);
    spdlog::info("{}: {} {}", core::versionBanner(), kProgram, command.name);
    spdlog::info("Asset root: {} ({})", core::pathToUtf8(paths->assetRoot()),
                 core::toString(paths->assetRootSource()));
    spdlog::info("User data: {}{}", core::pathToUtf8(paths->userDataRoot()), paths->isPortable() ? " (portable)" : "");

    ExitCode code = ExitCode::RuntimeFailure;
    try {
        CommandContext context{command, invocation.global, invocation.arguments, *paths, out, err};
        code = command.run(context);
    } catch (const std::exception& e) {
        spdlog::error("{} {}: unexpected failure: {}", kProgram, command.name, e.what());
    } catch (...) {
        spdlog::error("{} {}: unexpected failure: unknown exception", kProgram, command.name);
    }
    spdlog::info("{} {}: exit code {}", kProgram, command.name, toInt(code));
    core::shutdownLogging();
    return code;
}

} // namespace

Expected<Invocation> parseInvocation(std::span<const std::string> arguments) {
    // The global options end at the first argument that is not an option: the command's name.
    std::size_t commandIndex = 0;
    while (commandIndex < arguments.size()) {
        const std::string& argument = arguments[commandIndex];
        if (argument.size() < 2 || argument.front() != '-') {
            break;
        }
        ++commandIndex;
        const bool takesValue =
            std::find(kValueOptions.begin(), kValueOptions.end(), std::string_view(argument)) != kValueOptions.end();
        if (takesValue && commandIndex < arguments.size()) {
            ++commandIndex;
        }
    }

    const auto args = globalParser().parse(arguments.first(commandIndex));
    if (!args) {
        return propagate(args.error());
    }
    Invocation invocation;
    GlobalOptions& global = invocation.global;
    global.showHelp = args->has("help");
    global.showVersion = args->has("version");
    if (commandIndex < arguments.size()) {
        invocation.command = arguments[commandIndex];
        invocation.arguments.assign(arguments.begin() + static_cast<std::ptrdiff_t>(commandIndex) + 1,
                                    arguments.end());
    }
    if (global.showHelp || global.showVersion) {
        return invocation;
    }

    auto dataDir = pathOption(*args, "data-dir");
    if (!dataDir) {
        return propagate(dataDir.error());
    }
    auto assetRoot = pathOption(*args, "asset-root");
    if (!assetRoot) {
        return propagate(assetRoot.error());
    }
    global.dataDir = std::move(*dataDir);
    global.assetRoot = std::move(*assetRoot);
    global.portable = args->has("portable");
    if (global.dataDir && global.portable) {
        return makeError(ErrorCode::InvalidArgument, "use either --data-dir or --portable, not both", "--portable");
    }
    auto level = logLevel(*args);
    if (!level) {
        return propagate(level.error());
    }
    global.logLevel = *level;
    return invocation;
}

std::span<const Command> commands() noexcept {
    return kCommands;
}

const Command* findCommand(std::string_view name) noexcept {
    const auto it =
        std::find_if(kCommands.begin(), kCommands.end(), [&](const Command& command) { return command.name == name; });
    return it == kCommands.end() ? nullptr : &*it;
}

std::string helpText() {
    std::size_t nameWidth = 0;
    for (const Command& command : kCommands) {
        nameWidth = std::max(nameWidth, command.name.size());
    }
    std::string text = fmt::format(
        "Usage: {0} [--data-dir <dir> | --portable] [--asset-root <dir>] [--log-level <level>] <command> [options]\n"
        "       {0} --help | --version\n"
        "\n"
        "Reads an ES-DE collection into the Retroverse catalog, searches it and launches its games, without a window.\n"
        "Results go to stdout, or to the file a command's --output names, as UTF-8; logs go to stderr and to\n"
        "<user data>/logs/{1}.\n"
        "\n"
        "Commands:\n",
        kProgram, kLogFileName);
    for (const Command& command : kCommands) {
        text += fmt::format("  {:<{}}  {}\n", command.name, nameWidth, command.summary);
    }
    text += "\n"
            "Global options (before the command):\n"
            "  --data-dir <dir>     User data directory: configuration, database and logs\n"
            "                       (default: %LOCALAPPDATA%\\Retroverse).\n"
            "  --portable           Keep user data next to the executable (RetroverseData).\n"
            "  --asset-root <dir>   Asset directory (overrides RV_ASSET_ROOT and the defaults).\n"
            "  --log-level <level>  trace, debug, info, warn or error: the level of the log file (default info);\n"
            "                       stderr shows warnings and errors, and everything at debug and trace.\n"
            "  -h, --help           Show this help and exit.\n"
            "  --version            Print the version and exit.\n"
            "\n"
            "Exit codes: 0 ok; 1 a check found problems; 2 usage or initialization error; 3 runtime failure.\n";
    return text;
}

ExitCode notImplemented(CommandContext& context) {
    context.err.writeLine(fmt::format("{} {}: not implemented yet (I2 step {})", kProgram, context.command.name,
                                      context.command.step));
    return ExitCode::UsageError;
}

ExitCode run(std::span<const std::string> arguments, Output& out, Output& err,
             const core::PathEnvironment& environment) {
    const auto invocation = parseInvocation(arguments);
    if (!invocation) {
        err.writeLine(fmt::format("{}: {}", kProgram, invocation.error().toString()));
        err.writeLine(fmt::format("Run '{} --help' for the commands and the options.", kProgram));
        return ExitCode::UsageError;
    }
    if (invocation->global.showHelp) {
        out.write(helpText());
        return ExitCode::Ok;
    }
    if (invocation->global.showVersion) {
        out.writeLine(core::versionBanner());
        return ExitCode::Ok;
    }
    if (invocation->command.empty()) {
        err.write(helpText());
        return ExitCode::UsageError;
    }
    const Command* command = findCommand(invocation->command);
    if (command == nullptr) {
        err.writeLine(fmt::format("{}: unknown command '{}'", kProgram, invocation->command));
        err.writeLine(fmt::format("Run '{} --help' for the commands.", kProgram));
        return ExitCode::UsageError;
    }
    return runCommand(*command, *invocation, out, err, environment);
}

} // namespace rv::cli
