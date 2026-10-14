#include <rv/app/command_line.hpp>

#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>

namespace rv::app {
namespace {

constexpr std::string_view kDescription =
    "Explorable 3D frontend for ES-DE game collections.\n"
    "\n"
    "Keys: Esc quits, Alt+Enter toggles borderless fullscreen.\n"
    "Exit codes: 0 success; 1 --smoke run with unexpected OpenGL errors or a failed --gl-selftest;\n"
    "            2 initialization failure (command line, paths, SDL, window, OpenGL 4.5); 3 unexpected failure.";

core::ArgParser makeParser() {
    core::ArgParser parser("retroverse", std::string(kDescription));
    parser.flag("version", "Print the version and exit.")
        .option("width", "pixels", "Initial window width.", std::to_string(CommandLineOptions::kDefaultWindowWidth))
        .option("height", "pixels", "Initial window height.",
                std::to_string(CommandLineOptions::kDefaultWindowHeight))
        .flag("fullscreen", "Start in borderless fullscreen.")
        .flag("hidden", "Never show the window; only with --smoke or --exit-after-frames.")
        .flag("no-vsync", "Disable vertical synchronization.")
        .flag("smoke", "Render a few frames (see --frames) and exit; implies --no-dialogs; High-severity OpenGL "
                       "debug messages make the exit code 1.")
        .option("frames", "n", "Frames to render before exiting with --smoke (default 3) or --exit-after-frames.")
        .flag("exit-after-frames", "Exit after the number of frames given by --frames.")
        .flag("exit", "Same as --exit-after-frames.")
        .flag("gl-selftest", "Provoke one GL_INVALID_ENUM after initialization to prove that OpenGL errors reach "
                             "the log, then continue.")
        .option("asset-root", "dir", "Asset directory (overrides RV_ASSET_ROOT and the defaults).")
        .option("data-dir", "dir", "User data directory: config, cache and logs.")
        .flag("portable", "Keep user data next to the executable (RetroverseData).")
        .option("log-level", "level", "One of trace, debug, info, warn, error.", "info")
        .flag("no-dialogs", "Never show message boxes; failures are only logged.")
        .option("scene", "name", "Scene to load (not implemented yet).")
        .option("screenshot", "png", "Write a screenshot after the last frame (not implemented yet).")
        .option("model", "file", "glTF model to display (not implemented yet).");
    return parser;
}

Expected<int> windowExtent(const core::ParsedArgs& args, std::string_view name, int fallback) {
    auto value = args.intValue(name, fallback);
    if (!value) {
        return propagate(value.error());
    }
    if (*value < CommandLineOptions::kMinWindowExtent || *value > CommandLineOptions::kMaxWindowExtent) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("must be between {} and {}", CommandLineOptions::kMinWindowExtent,
                                     CommandLineOptions::kMaxWindowExtent),
                         fmt::format("--{}={}", name, *value));
    }
    return *value;
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

/// Non-empty option value, if the option was given.
Expected<std::optional<std::string>> nonEmptyValue(const core::ParsedArgs& args, std::string_view name) {
    if (!args.has(name)) {
        return std::optional<std::string>{};
    }
    std::string text = args.value(name).value_or("");
    if (text.empty()) {
        return makeError(ErrorCode::InvalidArgument, "value must not be empty", "--" + std::string(name));
    }
    return std::optional<std::string>(std::move(text));
}

Expected<std::optional<std::filesystem::path>> pathValue(const core::ParsedArgs& args, std::string_view name) {
    auto text = nonEmptyValue(args, name);
    if (!text) {
        return propagate(text.error());
    }
    if (!*text) {
        return std::optional<std::filesystem::path>{};
    }
    return std::optional<std::filesystem::path>(core::pathFromUtf8(**text));
}

/// --frames, --smoke and --exit-after-frames together decide when the run stops; --hidden needs such a limit.
Status applyRunLength(const core::ParsedArgs& args, CommandLineOptions& options) {
    if (args.has("frames")) {
        if (!options.smoke && !options.exitAfterFrames) {
            return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames", "--frames");
        }
        auto frames = args.intValue("frames", 0);
        if (!frames) {
            return propagate(frames.error());
        }
        if (*frames < 1) {
            return makeError(ErrorCode::OutOfRange, "must be at least 1", fmt::format("--frames={}", *frames));
        }
        options.frameLimit = static_cast<std::uint32_t>(*frames);
    } else if (options.smoke) {
        options.frameLimit = CommandLineOptions::kDefaultSmokeFrames;
    } else if (options.exitAfterFrames) {
        return makeError(ErrorCode::InvalidArgument, "requires --frames <n>", "--exit-after-frames");
    }
    // An invisible window cannot be closed by the user: such a run must end on its own.
    if (options.hidden && !options.frameLimit) {
        return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames", "--hidden");
    }
    return ok();
}

} // namespace

Expected<CommandLineOptions> parseCommandLine(std::span<const std::string> arguments) {
    const core::ArgParser parser = makeParser();
    const auto args = parser.parse(arguments);
    if (!args) {
        return propagate(args.error());
    }

    CommandLineOptions options;
    options.showHelp = args->has("help");
    options.showVersion = args->has("version");
    if (options.showHelp || options.showVersion) {
        return options;
    }

    auto width = windowExtent(*args, "width", CommandLineOptions::kDefaultWindowWidth);
    if (!width) {
        return propagate(width.error());
    }
    auto height = windowExtent(*args, "height", CommandLineOptions::kDefaultWindowHeight);
    if (!height) {
        return propagate(height.error());
    }
    options.windowWidth = *width;
    options.windowHeight = *height;
    options.fullscreen = args->has("fullscreen");
    options.hidden = args->has("hidden");
    options.vsync = !args->has("no-vsync");

    options.smoke = args->has("smoke");
    options.exitAfterFrames = args->has("exit-after-frames") || args->has("exit");
    if (auto runLength = applyRunLength(*args, options); !runLength) {
        return propagate(runLength.error());
    }
    options.glSelfTest = args->has("gl-selftest");

    auto assetRoot = pathValue(*args, "asset-root");
    if (!assetRoot) {
        return propagate(assetRoot.error());
    }
    auto dataDir = pathValue(*args, "data-dir");
    if (!dataDir) {
        return propagate(dataDir.error());
    }
    options.assetRoot = std::move(*assetRoot);
    options.dataDir = std::move(*dataDir);
    options.portable = args->has("portable");

    auto level = logLevel(*args);
    if (!level) {
        return propagate(level.error());
    }
    options.logLevel = *level;
    options.noDialogs = args->has("no-dialogs") || options.smoke;

    auto scene = nonEmptyValue(*args, "scene");
    if (!scene) {
        return propagate(scene.error());
    }
    auto screenshot = pathValue(*args, "screenshot");
    if (!screenshot) {
        return propagate(screenshot.error());
    }
    auto model = pathValue(*args, "model");
    if (!model) {
        return propagate(model.error());
    }
    options.scene = std::move(*scene);
    options.screenshot = std::move(*screenshot);
    options.model = std::move(*model);
    return options;
}

Expected<CommandLineOptions> parseCommandLine(int argc, const char* const* argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    return parseCommandLine(arguments);
}

std::string commandLineHelp() {
    return makeParser().helpText();
}

} // namespace rv::app
