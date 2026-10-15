// retroverse-screenshot-diff: compares a screenshot with its golden image within a tolerance (style guide §26.1, plan
// §7) and writes a heatmap of the differences when they do not match.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>
#include <rv/screenshot_diff/screenshot_diff.hpp>

namespace {

/// Exit codes of the tool.
constexpr int kMatch = 0;
constexpr int kMismatch = 1;
constexpr int kError = 2;

constexpr const char* kDescription =
    "Compares a screenshot with its golden image: a pixel differs when one of its color channels differs by more\n"
    "than the threshold, and the images match when at most the given share of their pixels differ (the defaults are\n"
    "the Gate A tolerance: 6/255 and 0.5%). Alpha is ignored. Images of different sizes never match.\n"
    "Heatmap: unchanged pixels show the expected image as dark grey, pixels within the threshold dim blue, pixels\n"
    "beyond it red to yellow (64/255 or more apart).\n"
    "Exit codes: 0 the images match; 1 they do not; 2 bad arguments or an unreadable image.";

rv::core::ArgParser makeParser() {
    const rv::screenshot_diff::Tolerance defaults;
    rv::core::ArgParser parser("retroverse-screenshot-diff", kDescription);
    parser.positional("expected", "Golden image (PNG or JPEG).", true)
        .positional("actual", "Image to check (PNG or JPEG).", true)
        .option("threshold", "0-255", "Largest channel difference that does not count.",
                std::to_string(defaults.channelThreshold))
        .option("max-percent", "0-100", "Largest share of differing pixels that still matches, in percent.",
                fmt::format("{:g}", defaults.maxDifferingPercent))
        .option("heatmap", "png", "Write a heatmap of the differences to this PNG file when the images do not match.");
    return parser;
}

int run(const std::vector<std::string>& arguments) {
    const rv::core::ArgParser parser = makeParser();
    const auto args = parser.parse(arguments);
    if (!args) {
        std::fprintf(stderr, "%s\n\n%s", args.error().toString().c_str(), parser.helpText().c_str());
        return kError;
    }
    if (args->has("help")) {
        std::printf("%s", parser.helpText().c_str());
        return kMatch;
    }
    rv::screenshot_diff::Tolerance tolerance;
    const auto threshold = args->intValue("threshold", tolerance.channelThreshold);
    const auto percent = args->doubleValue("max-percent", tolerance.maxDifferingPercent);
    if (!threshold || !percent) {
        std::fprintf(stderr, "%s\n", (!threshold ? threshold.error() : percent.error()).toString().c_str());
        return kError;
    }
    tolerance.channelThreshold = *threshold;
    tolerance.maxDifferingPercent = *percent;
    std::optional<std::filesystem::path> heatmap;
    if (const auto value = args->value("heatmap"); value && !value->empty()) {
        heatmap = rv::core::pathFromUtf8(*value);
    }
    const auto comparison =
        rv::screenshot_diff::compareFiles(rv::core::pathFromUtf8(args->positional("expected").value_or("")),
                                          rv::core::pathFromUtf8(args->positional("actual").value_or("")), tolerance,
                                          heatmap);
    if (!comparison) {
        std::fprintf(stderr, "%s\n", comparison.error().toString().c_str());
        return kError;
    }
    std::printf("%s\n", comparison->describe(tolerance).c_str());
    if (!comparison->matches && heatmap && comparison->sameSize()) {
        std::printf("Heatmap written to %s\n", rv::core::pathToUtf8(*heatmap).c_str());
    }
    return comparison->matches ? kMatch : kMismatch;
}

} // namespace

// wmain: the arguments arrive as UTF-16 and are converted to UTF-8, so paths outside the ANSI code page work.
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.push_back(rv::core::pathToUtf8(std::filesystem::path(argv[i])));
        }
        return run(arguments);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Unexpected failure: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Unexpected failure: unknown exception\n");
    }
    return kError;
}
