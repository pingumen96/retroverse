#include <rv/app/command_line.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/ids.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>
#include <rv/world/ids.hpp>

namespace rv::app {
namespace {

constexpr std::string_view kDescription =
    "Explorable 3D frontend for ES-DE game collections.\n"
    "\n"
    "Controls: WASD or arrows walk, Left Shift sprints, the mouse looks around while captured (at startup in\n"
    "          interactive runs, or after a click into the window); gamepad: left stick walks, right stick\n"
    "          looks, left stick click sprints. Tab (gamepad Y) opens the quick catalog (the ES-DE catalog that\n"
    "          retroverse-cli imported into the user data directory, or --mock-catalog's), where arrows or the\n"
    "          D-pad move and Enter (A) launches the selected game. M (gamepad Select) opens the directory of the\n"
    "          building's floors, rooms, favourites and recent games anywhere in free roam, E (gamepad A) facing an\n"
    "          elevator too; choosing a floor or a room rides or walks there. Esc (gamepad B) leaves one\n"
    "          level: it closes a popup of the developer overlay, else the game panel, else steps back through\n"
    "          the quick catalog or the directory and closes it, else releases the mouse, else closes the overlay,\n"
    "          else quits. F1 toggles the developer overlay (developer builds), F5 reloads the shaders, F12\n"
    "          saves a screenshot to <user data>/screenshots. These follow the bindings of\n"
    "          assets/config/default_bindings.json, overridden per action by <user data>/config/bindings.json;\n"
    "          Alt+Enter (borderless fullscreen) is fixed. Retroverse never ends an emulator: quitting while a\n"
    "          game runs leaves it running.\n"
    "Exit codes: 0 success; 1 --smoke run that stopped early, hit unexpected OpenGL errors or failed --gl-selftest;\n"
    "            2 initialization failure (command line, paths, SDL, window, OpenGL 4.5, scene file, input script,\n"
    "              mock catalog);\n"
    "            3 runtime failure (unexpected error, --screenshot not written, scene shader never built,\n"
    "              --self-check failed, developer overlay asked for by a frame-limited run unable to start,\n"
    "              --run-report not written).";

core::ArgParser makeParser() {
    core::ArgParser parser("retroverse", std::string(kDescription));
    addHostOptions(parser);
    parser
        .option("scene", "name|file",
                fmt::format("Scene to load: a name from <asset root>/{} (default {}) or the path of a scene file.",
                            kSceneDirectory, kDefaultSceneName))
        .option("input-script", "json",
                "Replace the input of chosen frames with scripted action values, a JSON array of steps such as "
                "[{\"fromFrame\": 0, \"toFrame\": 119, \"actions\": {\"move_forward\": 1}}] (frames count from 0, "
                "both included); a step's \"phase\": \"<name>\" holds a named phase open on its frames, whose frame "
                "times the run report gives apart.")
        .option("model", "file",
                "glTF model (.gltf or .glb) to inspect instead of a scene: drawn at the origin with a three-point "
                "light rig and a camera framing it. Not together with --scene.")
        .option("spawn", "x,y,z[,yaw[,pitch]]",
                "Start the walker with its feet at (x, y, z) meters instead of at the scene camera, looking along yaw "
                "and pitch degrees (the scene camera's when left out; pitch within +-89), e.g. 0,0,3.2,0,-6: the "
                "feet, yaw and pitch that frame-limited runs log as their final camera.")
        .flag("debug-draw", "Draw debug lines over the scene: object bounds, socket axes and a world grid.")
        .option("self-check", "name",
                "Run a rendering check after initialization and fail the run (exit code 3) if it does not pass; "
                "only with --smoke or --exit-after-frames. srgb: an unlit grey quad that the tonemap shows as linear "
                "0.5, drawn into an HDR target and taken through the post-processing of the views (bloom, vignette, "
                "dithering and anti-aliasing off, then bloom, dithering and anti-aliasing on), must be stored as sRGB "
                "188 (+-2) by the sRGB output target. bloom: a lit surface below the bloom threshold must not bloom, "
                "an emitter must keep its energy (+-5%) and a single very bright pixel must glow alike (+-20%) "
                "wherever it falls. fxaa: a hard slanted edge between black and white, post-processed without "
                "dithering, must get intermediate values along the edge from the anti-aliasing while its flat areas "
                "are copied exactly.")
        .flag("devui-open", "Show the developer overlay (F1) from the first frame; developer builds only. A "
                            "frame-limited run fails (exit code 3) when the overlay cannot start.")
        .flag("screenshot-include-ui",
              "Draw the developer overlay, when open, into --screenshot and F12 screenshots too (they show the scene "
              "only by default), scaled uniformly to fit the screenshot and centered. A frame-limited run fails (exit "
              "code 3) when the overlay cannot start.")
        .option("mock-catalog", "json",
                "Games of the quick catalog (Tab or gamepad Y opens it) instead of the ES-DE catalog of the user data "
                "directory: invented, each with a launch target, an emulator profile and a ROM (a JSON file, "
                "schemaVersion 1, with \"profiles\" and \"games\"; for tests). Avvia launches the selected game; "
                "Retroverse renders nothing until the emulator returns.")
        .option("launch-wait-limit", "seconds",
                fmt::format("Only with --smoke or --exit-after-frames: a launched game still running after this many "
                            "seconds is left running (never ended) and the run goes on to its --frames (default {:g}, "
                            "at most {:g}).",
                            CommandLineOptions::kDefaultLaunchWaitLimitSeconds,
                            CommandLineOptions::kMaxLaunchWaitLimitSeconds))
        .flag("refresh-library", "Refresh the catalog from ES-DE once it is open, as \"Aggiorna libreria\" does in the "
                                 "quick catalog: the import of 'retroverse-cli import' on the catalog worker, the "
                                 "list usable meanwhile (the run report's catalog.refreshes).")
        .option("video-freeze-pts", "seconds",
                "Only with --smoke or --exit-after-frames: the hero CRT's video seeks to this time and holds the "
                "frame, for goldens.")
        .option("max-decoders", "1|2",
                "Video decoders the screens share at most (video.json's maxDecoders by default: 2, the Balanced "
                "profile; 1 is the Low profile's): the screens beyond it show their stills.")
        .flag("no-texture-compression", "Keep the collection's covers and spines in RGBA8 at half the resolution "
                                        "instead of BC1, as without the S3TC extensions (the log names the format "
                                        "and why).")
        .option("start-room", "room",
                "Start in this room of the building (its id: room:snes, area:arcade, ...) at its doorway camera, on "
                "its floor, instead of in the ground floor's lobby; --spawn and the run report's poses are then in "
                "that room's frame. The run exits 2 when the building's plan lacks the room. Not with --scene, "
                "--model or --mock-catalog.");
    return parser;
}

bool isAsciiDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

/// Strictly decimal, unsigned and without surrounding text; nothing on overflow.
std::optional<int> parseDecimal(std::string_view text) noexcept {
    if (text.empty() || text.size() > 9) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        if (!isAsciiDigit(c)) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

/// --screenshot-size WIDTHxHEIGHT (the 'x' may be uppercase), each side in [1, kMaxScreenshotExtent].
Status applyScreenshotSize(const core::ParsedArgs& args, CommandLineOptions& options) {
    if (!args.has("screenshot-size")) {
        return ok();
    }
    const std::string text = args.value("screenshot-size").value_or("");
    const std::string context = "--screenshot-size=" + text;
    const std::size_t separator = text.find_first_of("xX");
    if (separator == std::string::npos) {
        return makeError(ErrorCode::ParseError, "expected WIDTHxHEIGHT, e.g. 1920x1080", context);
    }
    const std::optional<int> width = parseDecimal(std::string_view(text).substr(0, separator));
    const std::optional<int> height = parseDecimal(std::string_view(text).substr(separator + 1));
    if (!width || !height) {
        return makeError(ErrorCode::ParseError, "expected WIDTHxHEIGHT, e.g. 1920x1080", context);
    }
    const auto inRange = [](int extent) {
        return extent >= 1 && extent <= CommandLineOptions::kMaxScreenshotExtent;
    };
    if (!inRange(*width) || !inRange(*height)) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("each side must be between 1 and {}", CommandLineOptions::kMaxScreenshotExtent),
                         context);
    }
    options.screenshotWidth = *width;
    options.screenshotHeight = *height;
    return ok();
}

/// A --scene name becomes a file name under the asset root: keep it to a portable, harmless character set.
Status validateSceneName(std::string_view name) {
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isAsciiDigit(c) || c == '_' || c == '-' || c == '.';
    };
    if (name.front() == '.' || !std::all_of(name.begin(), name.end(), allowed)) {
        return makeError(ErrorCode::InvalidArgument,
                         "scene names may only contain letters, digits, '_', '-' and '.' and must not start with '.'; "
                         "pass a path to load another file",
                         "--scene=" + std::string(name));
    }
    return ok();
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

/// --fixed-dt <seconds>: a finite number in (0, kMaxFixedDeltaSeconds].
Expected<std::optional<double>> fixedDeltaSeconds(const core::ParsedArgs& args) {
    if (!args.has("fixed-dt")) {
        return std::optional<double>{};
    }
    auto seconds = args.doubleValue("fixed-dt", 0.0);
    if (!seconds) {
        return propagate(seconds.error());
    }
    if (!std::isfinite(*seconds) || *seconds <= 0.0 || *seconds > CommandLineOptions::kMaxFixedDeltaSeconds) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("must be a number of seconds greater than 0 and at most {}",
                                     CommandLineOptions::kMaxFixedDeltaSeconds),
                         "--fixed-dt=" + args.value("fixed-dt").value_or(""));
    }
    return std::optional<double>(*seconds);
}

/// --launch-wait-limit <seconds>: a finite number in (0, kMaxLaunchWaitLimitSeconds], frame-limited runs only, where it
/// defaults to kDefaultLaunchWaitLimitSeconds.
Status applyLaunchWaitLimit(const core::ParsedArgs& args, CommandLineOptions& options) {
    if (!args.has("launch-wait-limit")) {
        if (options.frameLimit) {
            options.launchWaitLimitSeconds = CommandLineOptions::kDefaultLaunchWaitLimitSeconds;
        }
        return ok();
    }
    // An interactive run waits for the user's game as long as it runs.
    if (!options.frameLimit) {
        return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames",
                         "--launch-wait-limit");
    }
    auto seconds = args.doubleValue("launch-wait-limit", 0.0);
    if (!seconds) {
        return propagate(seconds.error());
    }
    if (!std::isfinite(*seconds) || *seconds <= 0.0 || *seconds > CommandLineOptions::kMaxLaunchWaitLimitSeconds) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("must be a number of seconds greater than 0 and at most {:g}",
                                     CommandLineOptions::kMaxLaunchWaitLimitSeconds),
                         "--launch-wait-limit=" + args.value("launch-wait-limit").value_or(""));
    }
    options.launchWaitLimitSeconds = *seconds;
    return ok();
}

/// --video-freeze-pts <seconds>, a time in the clip of a frame-limited run.
Status applyVideoFreezePts(const core::ParsedArgs& args, CommandLineOptions& options) {
    if (!args.has("video-freeze-pts")) {
        return ok();
    }
    // An interactive run's video plays: holding a frame is for screenshots and goldens.
    if (!options.frameLimit) {
        return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames",
                         "--video-freeze-pts");
    }
    auto seconds = args.doubleValue("video-freeze-pts", 0.0);
    if (!seconds) {
        return propagate(seconds.error());
    }
    if (!std::isfinite(*seconds) || *seconds < 0.0 || *seconds > CommandLineOptions::kMaxVideoFreezePtsSeconds) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("must be a number of seconds from 0 to {:g}",
                                     CommandLineOptions::kMaxVideoFreezePtsSeconds),
                         "--video-freeze-pts=" + args.value("video-freeze-pts").value_or(""));
    }
    options.videoFreezePtsSeconds = *seconds;
    return ok();
}

/// --max-decoders <1|2>, the screens' decoder budget.
Status applyMaxDecoders(const core::ParsedArgs& args, CommandLineOptions& options) {
    if (!args.has("max-decoders")) {
        return ok();
    }
    auto decoders = args.intValue("max-decoders", 0);
    if (!decoders) {
        return propagate(decoders.error());
    }
    if (*decoders < 1 || *decoders > 2) {
        return makeError(ErrorCode::OutOfRange, "must be 1 or 2", fmt::format("--max-decoders={}", *decoders));
    }
    options.maxDecoders = *decoders;
    return ok();
}

/// --spawn x,y,z[,yaw[,pitch]], when given.
Expected<std::optional<SpawnPoint>> spawnValue(const core::ParsedArgs& args) {
    if (!args.has("spawn")) {
        return std::optional<SpawnPoint>{};
    }
    auto spawn = parseSpawnPoint(args.value("spawn").value_or(""));
    if (!spawn) {
        return propagate(spawn.error());
    }
    return std::optional<SpawnPoint>(*spawn);
}

/// --model <file>: a glTF file, shown instead of a scene.
Status validateModel(const std::filesystem::path& model, bool sceneGiven) {
    const std::string extension = core::pathToUtf8(model.extension());
    if (!core::equalsIgnoreCaseAscii(extension, ".gltf") && !core::equalsIgnoreCaseAscii(extension, ".glb")) {
        return makeError(ErrorCode::InvalidArgument, "glTF models are .gltf or .glb files",
                         "--model=" + core::pathToUtf8(model));
    }
    if (sceneGiven) {
        return makeError(ErrorCode::InvalidArgument, "the model viewer replaces the scene: give --model or --scene",
                         "--model");
    }
    return ok();
}

/// --self-check <name>: a known check, in a run that ends on its own.
Status applySelfCheck(const core::ParsedArgs& args, CommandLineOptions& options) {
    if (!args.has("self-check")) {
        return ok();
    }
    const std::string name = args.value("self-check").value_or("");
    const auto check = selfCheckFromString(name);
    if (!check) {
        return makeError(ErrorCode::InvalidArgument, "unknown check (expected srgb, bloom or fxaa)",
                         "--self-check=" + name);
    }
    if (!options.frameLimit) {
        return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames", "--self-check");
    }
    options.selfCheck = *check;
    return ok();
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
    } else if (options.smoke || options.exitAfterFrames) {
        options.frameLimit = CommandLineOptions::kDefaultFrameLimit;
    }
    // An invisible window cannot be closed by the user: such a run must end on its own.
    if (options.hidden && !options.frameLimit) {
        return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames", "--hidden");
    }
    return ok();
}

} // namespace

void addHostOptions(core::ArgParser& parser) {
    parser.flag("version", "Print the version and exit.")
        .option("width", "pixels", "Initial window width.", std::to_string(CommandLineOptions::kDefaultWindowWidth))
        .option("height", "pixels", "Initial window height.", std::to_string(CommandLineOptions::kDefaultWindowHeight))
        .flag("fullscreen", "Start in borderless fullscreen.")
        .flag("hidden", "Never show the window; only with --smoke or --exit-after-frames.")
        .flag("no-vsync", "Disable vertical synchronization.")
        .flag("smoke", "Render a few frames (see --frames) and exit. The exit code is 1 when the run stops early or, "
                       "in builds with GL debug output, when High-severity OpenGL debug messages occur.")
        .option("frames", "n",
                fmt::format("Frames to render before exiting with --smoke or --exit-after-frames (default {}).",
                            CommandLineOptions::kDefaultFrameLimit))
        .flag("exit-after-frames", "Exit after the number of frames given by --frames.")
        .flag("exit", "Same as --exit-after-frames.")
        .flag("gl-selftest", "Provoke one GL_INVALID_ENUM after initialization to prove that OpenGL errors reach "
                             "the log, then continue.")
        .option("asset-root", "dir", "Asset directory (overrides RV_ASSET_ROOT and the defaults).")
        .option("data-dir", "dir", "User data directory: config, cache and logs.")
        .flag("portable", "Keep user data next to the executable (RetroverseData).")
        .option("log-level", "level", "One of trace, debug, info, warn, error.", "info")
        .flag("no-dialogs", "Never show message boxes; failures are only logged. Implied by --smoke and "
                            "--exit-after-frames.")
        .option("screenshot", "png", "Write a PNG of the last frame; only with --smoke or --exit-after-frames.")
        .option("screenshot-size", "WxH",
                fmt::format("Resolution of --screenshot and F12 screenshots, independent of the window (default "
                            "{}x{}).",
                            CommandLineOptions::kDefaultScreenshotWidth, CommandLineOptions::kDefaultScreenshotHeight))
        .option("fixed-dt", "seconds",
                fmt::format("Advance the simulation by this time step every frame instead of the measured frame time "
                            "(at most {} s), for reproducible automated runs, e.g. 0.016667.",
                            CommandLineOptions::kMaxFixedDeltaSeconds))
        .option("run-report", "json",
                "Write a JSON report of the run to this file when it ends, also when it fails (not when the command "
                "line is invalid): exit code and failure, OpenGL renderer and driver, GL debug message counts, the "
                "GL self-test, frames, the last view's statistics, the screenshot, the final camera, the self-check, "
                "the frame times of the run and its phases, the main thread's CPU sections, the GPU memory per kind "
                "(for automated checks such as tests/smoke).");
}

Status applyHostOptions(const core::ParsedArgs& args, CommandLineOptions& options) {
    auto width = windowExtent(args, "width", CommandLineOptions::kDefaultWindowWidth);
    if (!width) {
        return propagate(width.error());
    }
    auto height = windowExtent(args, "height", CommandLineOptions::kDefaultWindowHeight);
    if (!height) {
        return propagate(height.error());
    }
    options.windowWidth = *width;
    options.windowHeight = *height;
    options.fullscreen = args.has("fullscreen");
    options.hidden = args.has("hidden");
    options.vsync = !args.has("no-vsync");

    options.smoke = args.has("smoke");
    options.exitAfterFrames = args.has("exit-after-frames") || args.has("exit");
    if (auto runLength = applyRunLength(args, options); !runLength) {
        return propagate(runLength.error());
    }
    options.glSelfTest = args.has("gl-selftest");

    auto assetRoot = pathValue(args, "asset-root");
    if (!assetRoot) {
        return propagate(assetRoot.error());
    }
    auto dataDir = pathValue(args, "data-dir");
    if (!dataDir) {
        return propagate(dataDir.error());
    }
    options.assetRoot = std::move(*assetRoot);
    options.dataDir = std::move(*dataDir);
    options.portable = args.has("portable");

    auto level = logLevel(args);
    if (!level) {
        return propagate(level.error());
    }
    options.logLevel = *level;
    // A frame-limited run may be unattended (scripts, agents): a modal message box would keep it from ever exiting.
    options.noDialogs = args.has("no-dialogs") || options.frameLimit.has_value();

    auto screenshot = pathValue(args, "screenshot");
    if (!screenshot) {
        return propagate(screenshot.error());
    }
    if (*screenshot) {
        // Only a frame-limited run has a last frame to capture.
        if (!options.frameLimit) {
            return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames",
                             "--screenshot");
        }
        if (!core::equalsIgnoreCaseAscii(core::pathToUtf8((*screenshot)->extension()), ".png")) {
            return makeError(ErrorCode::InvalidArgument, "screenshots are PNG files: use the .png extension",
                             "--screenshot=" + core::pathToUtf8(**screenshot));
        }
    }
    if (auto size = applyScreenshotSize(args, options); !size) {
        return propagate(size.error());
    }
    auto fixedDt = fixedDeltaSeconds(args);
    if (!fixedDt) {
        return propagate(fixedDt.error());
    }
    auto runReport = pathValue(args, "run-report");
    if (!runReport) {
        return propagate(runReport.error());
    }
    if (*runReport && !core::equalsIgnoreCaseAscii(core::pathToUtf8((*runReport)->extension()), ".json")) {
        return makeError(ErrorCode::InvalidArgument, "run reports are JSON files: use the .json extension",
                         "--run-report=" + core::pathToUtf8(**runReport));
    }
    options.screenshot = std::move(*screenshot);
    options.fixedDeltaSeconds = *fixedDt;
    options.runReport = std::move(*runReport);
    return ok();
}

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
    if (auto host = applyHostOptions(*args, options); !host) {
        return propagate(host.error());
    }

    auto scene = nonEmptyValue(*args, "scene");
    if (!scene) {
        return propagate(scene.error());
    }
    if (*scene && isSceneName(**scene)) {
        if (auto name = validateSceneName(**scene); !name) {
            return propagate(name.error());
        }
    }
    auto inputScript = pathValue(*args, "input-script");
    if (!inputScript) {
        return propagate(inputScript.error());
    }
    auto model = pathValue(*args, "model");
    if (!model) {
        return propagate(model.error());
    }
    if (*model) {
        if (auto valid = validateModel(**model, scene->has_value()); !valid) {
            return propagate(valid.error());
        }
    }
    if (auto selfCheck = applySelfCheck(*args, options); !selfCheck) {
        return propagate(selfCheck.error());
    }
    auto spawn = spawnValue(*args);
    if (!spawn) {
        return propagate(spawn.error());
    }
    auto mockCatalog = pathValue(*args, "mock-catalog");
    if (!mockCatalog) {
        return propagate(mockCatalog.error());
    }
    if (*mockCatalog && !core::equalsIgnoreCaseAscii(core::pathToUtf8((*mockCatalog)->extension()), ".json")) {
        return makeError(ErrorCode::InvalidArgument, "the mock catalog is a JSON file: use the .json extension",
                         "--mock-catalog=" + core::pathToUtf8(**mockCatalog));
    }
    if (auto waitLimit = applyLaunchWaitLimit(*args, options); !waitLimit) {
        return propagate(waitLimit.error());
    }
    if (auto freeze = applyVideoFreezePts(*args, options); !freeze) {
        return propagate(freeze.error());
    }
    if (auto decoders = applyMaxDecoders(*args, options); !decoders) {
        return propagate(decoders.error());
    }
    options.refreshLibrary = args->has("refresh-library");
    options.noTextureCompression = args->has("no-texture-compression");
    auto startRoom = nonEmptyValue(*args, "start-room");
    if (!startRoom) {
        return propagate(startRoom.error());
    }
    if (*startRoom) {
        if (!world::RoomId::parse(**startRoom)) {
            return makeError(ErrorCode::InvalidArgument, "not a room id (room:<system>, area:shared:<floor>, ...)",
                             "--start-room=" + **startRoom);
        }
        if (scene->has_value() || model->has_value() || mockCatalog->has_value()) {
            return makeError(ErrorCode::InvalidArgument,
                             "the building's start room needs the generated building: not with --scene, --model or "
                             "--mock-catalog",
                             "--start-room=" + **startRoom);
        }
        options.startRoom = **startRoom;
    }
    if (auto pending = pendingOptionError(options); !pending) {
        return propagate(pending.error());
    }
    options.spawn = *spawn;
    options.scene = std::move(*scene);
    options.inputScript = std::move(*inputScript);
    options.model = std::move(*model);
    options.debugDraw = args->has("debug-draw");
    options.devUiOpen = args->has("devui-open");
    options.screenshotIncludeUi = args->has("screenshot-include-ui");
    options.mockCatalog = std::move(*mockCatalog);
    return options;
}

Status pendingOptionError(const CommandLineOptions& options) {
    // One block per option, apart, so that the step implementing one removes its block without touching the others.
    (void)options; // every option is implemented since I3 steps 11 and 13r
    return ok();
}

Expected<SpawnPoint> parseSpawnPoint(std::string_view text) {
    const std::string context = "--spawn=" + std::string(text);
    const std::vector<std::string_view> fields = core::split(text, ',');
    if (fields.size() < 3 || fields.size() > 5) {
        return makeError(ErrorCode::ParseError, "expected x,y,z, x,y,z,yaw or x,y,z,yaw,pitch", context);
    }
    std::array<float, 5> values{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::string_view field = core::trim(fields[i]);
        const char* end = field.data() + field.size();
        const auto [ptr, ec] = std::from_chars(field.data(), end, values[i]);
        if (!field.empty() && ptr == end && ec == std::errc::result_out_of_range) {
            return makeError(ErrorCode::OutOfRange, fmt::format("value {} is out of range", i + 1), context);
        }
        if (field.empty() || ec != std::errc{} || ptr != end) {
            return makeError(ErrorCode::ParseError,
                             fmt::format("value {} (\"{}\") is not a decimal number", i + 1, field), context);
        }
        if (!std::isfinite(values[i])) {
            return makeError(ErrorCode::OutOfRange, fmt::format("value {} is not finite", i + 1), context);
        }
    }
    SpawnPoint spawn;
    spawn.feet = glm::vec3(values[0], values[1], values[2]);
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(spawn.feet[axis]) > SpawnPoint::kMaxCoordinate) {
            return makeError(ErrorCode::OutOfRange,
                             fmt::format("coordinates must be within +-{:g} m", SpawnPoint::kMaxCoordinate), context);
        }
    }
    if (fields.size() >= 4) {
        spawn.yawDeg = values[3];
    }
    if (fields.size() == 5) {
        if (std::fabs(values[4]) > SpawnPoint::kMaxPitchDeg) {
            return makeError(ErrorCode::OutOfRange,
                             fmt::format("the pitch must be within +-{:g} degrees", SpawnPoint::kMaxPitchDeg),
                             context);
        }
        spawn.pitchDeg = values[4];
    }
    return spawn;
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

bool isSceneName(std::string_view value) noexcept {
    if (value.find_first_of("/\\:") != std::string_view::npos) {
        return false;
    }
    constexpr std::string_view kExtension = ".json";
    return value.size() < kExtension.size() ||
           !core::equalsIgnoreCaseAscii(value.substr(value.size() - kExtension.size()), kExtension);
}

std::filesystem::path resolveScenePath(const std::optional<std::string>& scene,
                                       const std::filesystem::path& assetRoot) {
    const std::string_view value = scene ? std::string_view(*scene) : kDefaultSceneName;
    if (!isSceneName(value)) {
        return core::pathFromUtf8(value);
    }
    return (assetRoot / core::pathFromUtf8(kSceneDirectory) / core::pathFromUtf8(std::string(value) + ".json"))
        .lexically_normal();
}

} // namespace rv::app
