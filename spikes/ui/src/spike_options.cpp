#include "spike_options.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>

namespace rv::spike_ui {
namespace {

constexpr std::string_view kScreenshotAt = "--screenshot-at";

constexpr std::string_view kDescription =
    "Product UI spike (I1 spike d, docs/spikes/i1-ui.md, ADR-0005; I2 step 6, docs/i2/step-6.md): the quick catalog\n"
    "screen in RmlUi over the test room, on a generated catalog of 100,000 games (a virtualized list), navigated with\n"
    "the gamepad alone through the UI actions of the action map (assets/config/default_bindings.json, overridden by\n"
    "<user data>/config/bindings.json). By default a scripted virtual gamepad (SDL virtual joystick; real pads are\n"
    "ignored) walks the screen (--scenario quick-catalog): the selection down 10 times, right into the card's "
    "actions,\n"
    "Avvia, Indietro, Y and X, stick holds, a resting stick and one crossing its threshold, a search restoring the "
    "list\n"
    "in its middle, filters and the chips row, a resize to 1280x720 and back, text scaling and high contrast, the\n"
    "shoulders' filters, End, the triggers' pages and Home, then idle, taps and navigation phases for the "
    "measurements.\n"
    "--scenario user-bindings checks that a user bindings.json reaches the UI "
    "(spikes/ui/fixtures/user_bindings.json).\n"
    "Every focus change is logged; the CPU and GPU times of the UI pass (p50/p99, quiet frames and frames with\n"
    "changes), its heap allocations and the checks go to --report.\n"
    "The window is 1920x1080 unless --width or --height is given (a side not given then takes the default below).\n"
    "Controls with --interactive: D-pad, left stick or arrows move the focus, A/Enter activates, B/Esc goes back,\n"
    "Y Vedi media, X Informazioni, the triggers or Page Up/Down page, the stick clicks or Home/End jump, the "
    "shoulders\n"
    "change the filter; with the catalog closed Y reopens it and B quits.\n"
    "Exit codes: 0 every check passed; 1 a --smoke run stopped early or hit an OpenGL error; 2 initialization "
    "failure;\n"
    "3 a check failed or never ran, the selection did not end at --expect-selection, or the run failed.";

core::ArgParser makeParser() {
    core::ArgParser parser("spike_ui", std::string(kDescription));
    app::addHostOptions(parser);
    parser
        .option("screenshot-at", "frame:png",
                "Write a PNG of the window's frame (scene and UI, at --screenshot-size) at this frame, counted from "
                "0; repeatable.")
        .option("report", "json", "Write the measurements and checks of the run to this JSON file when it ends.")
        .option("scene", "name|file", "Background scene (default m0_testroom).")
        .option("ui-scale", "factor", "Text scaling of the UI, 0.5 to 3 (default 1).")
        .flag("high-contrast", "Start with the high-contrast theme.")
        .option("fps-cap", "n",
                "Pace the frames at n per second, as a display with vsync would (0, the default: as fast as possible). "
                "Use 60 for measurements with --no-vsync.")
        .flag("interactive", "No scripted gamepad: drive the screen by hand.")
        .option("scenario", "name",
                "The scripted scenario: quick-catalog (default) or user-bindings (expects the user bindings of "
                "spikes/ui/fixtures/user_bindings.json in <data dir>/config/bindings.json).")
        .option(
            "input-script", "json",
            "No scripted gamepad: UI actions (ui_down, ui_nav_y, ui_end, ...) replace the devices' on chosen frames "
            "(tests/input_scripts).")
        .option("expect-selection", "position",
                "With --input-script: fail (exit code 3) unless the selection ends at this position of the list.");
    return parser;
}

/// Parses "<frame>:<file.png>".
Expected<ScreenshotRequest> parseScreenshotAt(std::string_view value) {
    const std::size_t colon = value.find(':');
    const std::string context = std::string(kScreenshotAt) + "=" + std::string(value);
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= value.size()) {
        return makeError(ErrorCode::ParseError, "expected <frame>:<file.png>", context);
    }
    std::uint32_t frame = 0;
    const std::string_view number = value.substr(0, colon);
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), frame);
    if (error != std::errc() || end != number.data() + number.size()) {
        return makeError(ErrorCode::ParseError, "the frame is not a whole number", context);
    }
    std::filesystem::path path = core::pathFromUtf8(value.substr(colon + 1));
    if (!core::equalsIgnoreCaseAscii(core::pathToUtf8(path.extension()), ".png")) {
        return makeError(ErrorCode::InvalidArgument, "screenshots are PNG files: use the .png extension", context);
    }
    return ScreenshotRequest{frame, std::move(path)};
}

} // namespace

Expected<SpikeOptions> parseSpikeOptions(std::span<const std::string> arguments) {
    SpikeOptions options;
    // --screenshot-at repeats, which the parser's options do not: they are taken out first.
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        std::optional<std::string_view> value;
        if (argument == kScreenshotAt) {
            if (index + 1 >= arguments.size()) {
                return makeError(ErrorCode::ParseError, "missing value", std::string(kScreenshotAt));
            }
            value = arguments[++index];
        } else if (argument.starts_with(std::string(kScreenshotAt) + "=")) {
            value = std::string_view(argument).substr(kScreenshotAt.size() + 1);
        }
        if (!value) {
            rest.push_back(argument);
            continue;
        }
        auto request = parseScreenshotAt(*value);
        if (!request) {
            return propagate(request.error());
        }
        options.screenshots.push_back(std::move(*request));
    }

    const core::ArgParser parser = makeParser();
    auto args = parser.parse(rest);
    if (!args) {
        return propagate(args.error());
    }
    options.run.showHelp = args->has("help");
    options.run.showVersion = args->has("version");
    if (options.run.showHelp || options.run.showVersion) {
        return options;
    }
    if (auto host = app::applyHostOptions(*args, options.run); !host) {
        return propagate(host.error());
    }
    if (!args->has("width") && !args->has("height")) {
        options.run.windowWidth = kSpikeWindowWidth;
        options.run.windowHeight = kSpikeWindowHeight;
    }
    if (const std::optional<std::string> scene = args->value("scene")) {
        if (scene->empty()) {
            return makeError(ErrorCode::InvalidArgument, "empty scene", "--scene");
        }
        options.run.scene = *scene;
    }
    if (const std::optional<std::string> report = args->value("report")) {
        std::filesystem::path path = core::pathFromUtf8(*report);
        if (!core::equalsIgnoreCaseAscii(core::pathToUtf8(path.extension()), ".json")) {
            return makeError(ErrorCode::InvalidArgument, "the report is a JSON file: use the .json extension",
                             "--report=" + *report);
        }
        options.report = std::move(path);
    }
    auto scale = args->doubleValue("ui-scale", 1.0);
    if (!scale) {
        return propagate(scale.error());
    }
    if (!(*scale >= 0.5 && *scale <= 3.0)) {
        return makeError(ErrorCode::OutOfRange, "expected a factor from 0.5 to 3", "--ui-scale");
    }
    options.textScale = static_cast<float>(*scale);
    auto cap = args->intValue("fps-cap", 0);
    if (!cap) {
        return propagate(cap.error());
    }
    if (*cap < 0 || *cap > 1000) {
        return makeError(ErrorCode::OutOfRange, "expected 0 (no cap) to 1000 frames per second", "--fps-cap");
    }
    options.fpsCap = *cap;
    options.highContrast = args->has("high-contrast");
    const bool interactive = args->has("interactive");
    if (const std::optional<std::string> name = args->value("scenario")) {
        const std::optional<ScenarioKind> kind = scenarioKindFromName(*name);
        if (!kind) {
            return makeError(ErrorCode::InvalidArgument, "expected quick-catalog or user-bindings",
                             "--scenario=" + *name);
        }
        if (interactive || args->has("input-script")) {
            return makeError(ErrorCode::InvalidArgument,
                             "a scenario drives the virtual gamepad: not with --interactive or --input-script",
                             "--scenario");
        }
        options.scenarioKind = *kind;
    }
    if (const std::optional<std::string> script = args->value("input-script")) {
        if (interactive) {
            return makeError(ErrorCode::InvalidArgument, "not with --interactive", "--input-script");
        }
        if (script->empty()) {
            return makeError(ErrorCode::InvalidArgument, "empty path", "--input-script");
        }
        options.inputScript = core::pathFromUtf8(*script);
    }
    if (const std::optional<std::string> expected = args->value("expect-selection")) {
        std::size_t position = 0;
        const auto [end, error] = std::from_chars(expected->data(), expected->data() + expected->size(), position);
        if (error != std::errc() || end != expected->data() + expected->size()) {
            return makeError(ErrorCode::ParseError, "expected a position of the list (a whole number)",
                             "--expect-selection=" + *expected);
        }
        if (!options.inputScript || !options.run.frameLimit) {
            return makeError(ErrorCode::InvalidArgument,
                             "only with --input-script and a frame limit (--smoke or --exit-after-frames)",
                             "--expect-selection");
        }
        options.expectSelection = position;
    }
    options.scenario = !interactive && !options.inputScript;
    if ((options.scenario || options.inputScript) && !options.run.fixedDeltaSeconds) {
        // The scenario's checks and the input script count frames (stick repeats, holds): a fixed step makes them
        // independent of the speed of the machine. The rendering and its measurements do not depend on it.
        options.run.fixedDeltaSeconds = kScenarioFixedDeltaSeconds;
    }

    std::sort(options.screenshots.begin(), options.screenshots.end(),
              [](const ScreenshotRequest& a, const ScreenshotRequest& b) { return a.frame < b.frame; });
    const auto sameFrame = std::adjacent_find(
        options.screenshots.begin(), options.screenshots.end(),
        [](const ScreenshotRequest& a, const ScreenshotRequest& b) { return a.frame == b.frame; });
    if (sameFrame != options.screenshots.end()) {
        return makeError(ErrorCode::InvalidArgument, "one screenshot per frame",
                         std::string(kScreenshotAt) + "=" + std::to_string(sameFrame->frame));
    }
    for (const ScreenshotRequest& request : options.screenshots) {
        if (!options.run.frameLimit) {
            return makeError(ErrorCode::InvalidArgument, "only valid with --smoke or --exit-after-frames",
                             std::string(kScreenshotAt));
        }
        if (request.frame >= *options.run.frameLimit) {
            return makeError(ErrorCode::OutOfRange,
                             "the run ends before this frame (frames are counted from 0, below --frames)",
                             std::string(kScreenshotAt) + "=" + std::to_string(request.frame));
        }
    }
    return options;
}

Expected<SpikeOptions> parseSpikeOptions(int argc, const char* const* argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index] != nullptr ? argv[index] : "");
    }
    return parseSpikeOptions(arguments);
}

std::string spikeHelp() {
    return makeParser().helpText();
}

} // namespace rv::spike_ui
