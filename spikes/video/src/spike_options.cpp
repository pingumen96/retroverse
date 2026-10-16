#include "spike_options.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

#include <fmt/format.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>

namespace rv::spike {
namespace {

core::ArgParser makeParser() {
    core::ArgParser parser(
        "spike_video",
        "I1 spike (c): videos decoded with FFmpeg on worker threads (rv_media) and shown on textured quads through "
        "persistently mapped pixel buffers (gl::VideoTexture). Writes the measurements to a JSON report.\n"
        "  --videos <file>...            videos to play, read in place (several files after one --videos)");
    parser
        .option("generate-fixtures", "DIR",
                "write the test clips and the damaged files to DIR; plays the clips unless --videos is given")
        .flag("error-cases", "with --generate-fixtures: play the damaged files beside one good clip")
        .flag("regenerate", "write the fixtures even when files of the same name exist")
        .option("count", "N", "videos to play (clips generated, or the --videos list repeated), 1..16", "2")
        .option("fixture-size", "WxH", "size of the generated clips", "1280x720")
        .option("fixture-fps", "FPS", "frame rate of the generated clips", "30")
        .option("fixture-seconds", "S", "length of the generated clips (they loop)", "10")
        .option("fixture-codec", "CODEC", "mpeg4 or mjpeg", "mpeg4")
        .option("fixture-container", "EXT", "mp4, mkv or avi", "mp4")
        .option("seconds", "S", "length of the measured run, after the warm-up", "60")
        .option("warmup", "S", "seconds at the start left out of the statistics", "1")
        .option("max-height", "PIXELS", "frames taller than this are scaled down (0: no limit)", "720")
        .option("queue", "FRAMES", "frames each decoder decodes ahead", "4")
        .option("decoder-threads", "N", "FFmpeg threads per decoder", "1")
        .flag("upload-fences", "guard the pixel buffers with fences instead of the copies' end timestamps")
        .flag("check-frames",
              "read the frame counter of every uploaded test frame and of each picture in the screenshot (report: "
              "upload.lastCounter, upload.screenCounter)")
        .flag("texture-self-test",
              "before playing, check gl::VideoTexture's resize and busy-ring paths with exact readbacks (report: "
              "textureSelfTest)")
        .option("priority", "CLASS", "process priority: normal, above-normal or high", "normal")
        .option("report", "FILE.json", "write the measurements there")
        .option("screenshot", "FILE.png", "write the last frame there")
        .option("width", "PIXELS", "window width", "1920")
        .option("height", "PIXELS", "window height", "1080")
        .flag("no-vsync", "present without waiting for the display")
        .flag("hidden", "never show the window")
        .option("data-dir", "DIR", "user data directory (logs/spike-video.log)")
        .flag("list-encoders", "print the FFmpeg build and its video encoders, then exit")
        .option("log-level", "LEVEL", "trace, debug, info, warn or error", "info");
    return parser;
}

Error optionError(std::string_view option, std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message), std::string("--") + std::string(option)};
}

Expected<double> realIn(const core::ParsedArgs& args, std::string_view name, double fallback, double low, double high) {
    auto value = args.doubleValue(name, fallback);
    if (!value) {
        return propagate(optionError(name, value.error().message));
    }
    if (!std::isfinite(*value) || *value < low || *value > high) {
        return propagate(optionError(name, fmt::format("{} is outside [{}, {}]", *value, low, high)));
    }
    return *value;
}

Expected<int> integerIn(const core::ParsedArgs& args, std::string_view name, int fallback, int low, int high) {
    auto value = args.intValue(name, fallback);
    if (!value) {
        return propagate(optionError(name, value.error().message));
    }
    if (*value < low || *value > high) {
        return propagate(optionError(name, fmt::format("{} is outside [{}, {}]", *value, low, high)));
    }
    return *value;
}

Expected<std::pair<int, int>> sizeValue(const core::ParsedArgs& args, std::string_view name) {
    const std::string text = core::toLowerAscii(args.value(name).value_or(""));
    const auto parts = core::split(text, 'x');
    if (parts.size() == 2) {
        int width = 0;
        int height = 0;
        const auto [widthEnd, widthError] = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), width);
        const auto [heightEnd, heightError] =
            std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), height);
        if (widthError == std::errc{} && heightError == std::errc{} && widthEnd == parts[0].data() + parts[0].size() &&
            heightEnd == parts[1].data() + parts[1].size()) {
            return std::pair{width, height};
        }
    }
    return propagate(optionError(name, fmt::format("'{}' is not WIDTHxHEIGHT", text)));
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           core::equalsIgnoreCaseAscii(text.substr(text.size() - suffix.size()), suffix);
}

} // namespace

Expected<SpikeOptions> parseSpikeOptions(std::span<const std::string> arguments) {
    SpikeOptions options;
    // --videos takes every argument up to the next option; the rest goes to the parser.
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != "--videos") {
            rest.push_back(arguments[index]);
            continue;
        }
        while (index + 1 < arguments.size() && !arguments[index + 1].starts_with("--")) {
            options.videos.push_back(core::pathFromUtf8(arguments[++index]));
        }
        if (options.videos.empty()) {
            return propagate(optionError("videos", "needs at least one file"));
        }
    }
    const core::ArgParser parser = makeParser();
    auto parsed = parser.parse(rest);
    if (!parsed) {
        return propagate(parsed.error());
    }
    const core::ParsedArgs& args = *parsed;
    if (args.has("help")) {
        options.showHelp = true;
        return options;
    }
    if (args.has("list-encoders")) {
        options.listEncoders = true;
        return options;
    }

    if (const auto directory = args.value("generate-fixtures")) {
        if (directory->empty()) {
            return propagate(optionError("generate-fixtures", "needs a directory"));
        }
        options.fixtureDirectory = core::pathFromUtf8(*directory);
    }
    options.errorCases = args.has("error-cases");
    options.regenerate = args.has("regenerate");
    if (options.errorCases && !options.fixtureDirectory) {
        return propagate(optionError("error-cases", "needs --generate-fixtures"));
    }
    if (options.videos.empty() && !options.fixtureDirectory) {
        return propagate(optionError("videos", "nothing to play: give --videos or --generate-fixtures"));
    }

    auto count = integerIn(args, "count", 2, 1, 16);
    auto fixtureSize = sizeValue(args, "fixture-size");
    auto fixtureFps = integerIn(args, "fixture-fps", 30, 1, 240);
    auto fixtureSeconds = realIn(args, "fixture-seconds", 10.0, 0.1, 600.0);
    auto seconds = realIn(args, "seconds", 60.0, 0.1, 3600.0);
    auto warmup = realIn(args, "warmup", 1.0, 0.0, 60.0);
    auto maxHeight = integerIn(args, "max-height", 720, 0, 16384);
    auto queue = integerIn(args, "queue", 4, 1, 64);
    auto threads = integerIn(args, "decoder-threads", 1, 1, 16);
    auto width = integerIn(args, "width", SpikeOptions::kDefaultWidth, 64, 16384);
    auto height = integerIn(args, "height", SpikeOptions::kDefaultHeight, 64, 16384);
    for (const Error* error :
         {count ? nullptr : &count.error(), fixtureSize ? nullptr : &fixtureSize.error(),
          fixtureFps ? nullptr : &fixtureFps.error(), fixtureSeconds ? nullptr : &fixtureSeconds.error(),
          seconds ? nullptr : &seconds.error(), warmup ? nullptr : &warmup.error(),
          maxHeight ? nullptr : &maxHeight.error(), queue ? nullptr : &queue.error(),
          threads ? nullptr : &threads.error(), width ? nullptr : &width.error(), height ? nullptr : &height.error()}) {
        if (error != nullptr) {
            return propagate(*error);
        }
    }
    options.count = *count;
    options.countGiven = args.has("count");
    options.fixture.width = fixtureSize->first;
    options.fixture.height = fixtureSize->second;
    options.fixture.frameRate = *fixtureFps;
    options.fixture.durationSeconds = *fixtureSeconds;
    options.seconds = *seconds;
    options.warmupSeconds = *warmup;
    options.maxHeight = *maxHeight;
    options.queueCapacity = static_cast<std::size_t>(*queue);
    options.decoderThreads = *threads;
    options.uploadFences = args.has("upload-fences");
    options.checkFrames = args.has("check-frames");
    options.textureSelfTest = args.has("texture-self-test");
    options.priority = core::toLowerAscii(args.value("priority").value_or("normal"));
    if (options.priority != "normal" && options.priority != "above-normal" && options.priority != "high") {
        return propagate(
            optionError("priority", fmt::format("'{}' is not normal, above-normal or high", options.priority)));
    }

    const std::string codec = core::toLowerAscii(args.value("fixture-codec").value_or("mpeg4"));
    if (codec == "mpeg4") {
        options.fixture.codec = media::TestVideoCodec::Mpeg4;
    } else if (codec == "mjpeg") {
        options.fixture.codec = media::TestVideoCodec::Mjpeg;
    } else {
        return propagate(optionError("fixture-codec", fmt::format("'{}' is neither mpeg4 nor mjpeg", codec)));
    }
    const std::string container = core::toLowerAscii(args.value("fixture-container").value_or("mp4"));
    if (container != "mp4" && container != "mkv" && container != "avi") {
        return propagate(optionError("fixture-container", fmt::format("'{}' is not mp4, mkv or avi", container)));
    }
    options.fixtureExtension = "." + container;

    if (const auto report = args.value("report")) {
        if (!endsWith(*report, ".json")) {
            return propagate(optionError("report", "must end in .json"));
        }
        options.report = core::pathFromUtf8(*report);
    }
    if (const auto screenshot = args.value("screenshot")) {
        if (!endsWith(*screenshot, ".png")) {
            return propagate(optionError("screenshot", "must end in .png"));
        }
        options.screenshot = core::pathFromUtf8(*screenshot);
    }

    app::CommandLineOptions& host = options.host;
    host.windowWidth = *width;
    host.windowHeight = *height;
    host.hidden = args.has("hidden");
    host.vsync = !args.has("no-vsync");
    // Always an unattended, time-limited run: frame-limited for the host (no dialog, no wait while minimized); the
    // spike quits after its seconds.
    host.exitAfterFrames = true;
    host.frameLimit = std::numeric_limits<std::uint32_t>::max();
    host.noDialogs = true;
    if (const auto dataDir = args.value("data-dir")) {
        host.dataDir = core::pathFromUtf8(*dataDir);
    }
    const std::string level = core::toLowerAscii(args.value("log-level").value_or("info"));
    const spdlog::level::level_enum logLevel = spdlog::level::from_str(level);
    if (logLevel == spdlog::level::off && level != "off") {
        return propagate(optionError("log-level", fmt::format("'{}' is not a log level", level)));
    }
    host.logLevel = logLevel;
    return options;
}

std::string spikeHelp() {
    return makeParser().helpText() +
           "\nExit codes: 0 the run completed (videos that could not play fell back to a picture), 2 initialization "
           "failed, 3 a runtime failure (report or screenshot not written).\n";
}

} // namespace rv::spike
