#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <rv/app/command_line.hpp>
#include <rv/core/text.hpp>

using rv::ErrorCode;
using rv::app::CommandLineOptions;

namespace {

rv::Expected<CommandLineOptions> parse(const std::vector<std::string>& arguments) {
    return rv::app::parseCommandLine(arguments);
}

/// True when the help text has an entry for exactly `option` ("  --exit\n", "  --frames <n>\n"), so that an option
/// is not reported as listed merely because a longer one starts with the same characters.
bool helpListsOption(const std::string& help, const std::string& option) {
    return help.find("  " + option + "\n") != std::string::npos || help.find("  " + option + " <") != std::string::npos;
}

} // namespace

TEST_CASE("app/command_line: no arguments describe an interactive windowed run", "[app][command_line]") {
    const auto options = parse({});
    REQUIRE(options.has_value());
    CHECK_FALSE(options->showHelp);
    CHECK_FALSE(options->showVersion);
    CHECK(options->windowWidth == 1280);
    CHECK(options->windowHeight == 720);
    CHECK_FALSE(options->fullscreen);
    CHECK_FALSE(options->hidden);
    CHECK(options->vsync);
    CHECK_FALSE(options->smoke);
    CHECK_FALSE(options->exitAfterFrames);
    CHECK_FALSE(options->frameLimit.has_value());
    CHECK_FALSE(options->glSelfTest);
    CHECK_FALSE(options->assetRoot.has_value());
    CHECK_FALSE(options->dataDir.has_value());
    CHECK_FALSE(options->portable);
    CHECK(options->logLevel == spdlog::level::info);
    CHECK_FALSE(options->noDialogs);
    CHECK_FALSE(options->scene.has_value());
    CHECK_FALSE(options->screenshot.has_value());
    CHECK_FALSE(options->model.has_value());
}

TEST_CASE("app/command_line: --smoke renders three frames and never shows dialogs", "[app][command_line]") {
    const auto options = parse({"--smoke"});
    REQUIRE(options.has_value());
    CHECK(options->smoke);
    REQUIRE(options->frameLimit.has_value());
    CHECK(*options->frameLimit == CommandLineOptions::kDefaultFrameLimit);
    CHECK(*options->frameLimit == 3u);
    CHECK(options->noDialogs);
    CHECK_FALSE(options->hidden); // a smoke run still shows its window unless --hidden is given
    CHECK_FALSE(options->exitAfterFrames);
}

TEST_CASE("app/command_line: --frames sets the limit of smoke and exit-after-frames runs", "[app][command_line]") {
    SECTION("smoke") {
        const auto options = parse({"--smoke", "--frames", "5", "--gl-selftest"});
        REQUIRE(options.has_value());
        CHECK(options->frameLimit == 5u);
        CHECK(options->glSelfTest);
    }
    SECTION("exit after frames never shows dialogs") {
        const auto options = parse({"--frames=120", "--exit-after-frames"});
        REQUIRE(options.has_value());
        CHECK(options->exitAfterFrames);
        CHECK_FALSE(options->smoke);
        CHECK(options->frameLimit == 120u);
        CHECK(options->noDialogs);
    }
    SECTION("--exit is an alias of --exit-after-frames") {
        const auto options = parse({"--exit", "--frames", "2"});
        REQUIRE(options.has_value());
        CHECK(options->exitAfterFrames);
        CHECK(options->frameLimit == 2u);
    }
}

TEST_CASE("app/command_line: invalid frame limits are rejected with the option name", "[app][command_line]") {
    SECTION("--frames alone would never stop the run") {
        const auto options = parse({"--frames", "5"});
        REQUIRE_FALSE(options.has_value());
        CHECK(options.error().code == ErrorCode::InvalidArgument);
        CHECK(options.error().context == "--frames");
    }
    SECTION("zero and negative counts") {
        for (const char* count : {"0", "-2"}) {
            const auto options = parse({"--smoke", "--frames", count});
            REQUIRE_FALSE(options.has_value());
            CHECK(options.error().code == ErrorCode::OutOfRange);
            CHECK(options.error().context == std::string("--frames=") + count);
        }
    }
    SECTION("not a number") {
        const auto options = parse({"--smoke", "--frames", "three"});
        REQUIRE_FALSE(options.has_value());
        CHECK(options.error().code == ErrorCode::ParseError);
    }
}

TEST_CASE("app/command_line: --exit-after-frames without --frames renders the default frame count",
          "[app][command_line]") {
    for (const char* exitOption : {"--exit-after-frames", "--exit"}) {
        INFO(exitOption);
        const auto options = parse({exitOption});
        REQUIRE(options.has_value());
        CHECK(options->exitAfterFrames);
        CHECK_FALSE(options->smoke);
        REQUIRE(options->frameLimit.has_value());
        CHECK(*options->frameLimit == CommandLineOptions::kDefaultFrameLimit);
    }
    // The screenshot gate of later milestone steps passes --exit without --frames.
    const auto gate = parse({"--scene", "m0_testroom", "--screenshot", "build/G01.png", "--exit"});
    REQUIRE(gate.has_value());
    CHECK(gate->exitAfterFrames);
    CHECK(gate->frameLimit == CommandLineOptions::kDefaultFrameLimit);
}

TEST_CASE("app/command_line: --no-dialogs, and every frame-limited run, disable message boxes", "[app][command_line]") {
    const auto interactive = parse({"--no-dialogs"});
    REQUIRE(interactive.has_value());
    CHECK(interactive->noDialogs);
    CHECK_FALSE(interactive->smoke);
    CHECK_FALSE(interactive->exitAfterFrames);
    CHECK_FALSE(interactive->frameLimit.has_value());

    // Runs that end on their own may be unattended: a modal box would keep them from exiting.
    for (const auto& arguments : std::vector<std::vector<std::string>>{{"--smoke"},
                                                                       {"--exit"},
                                                                       {"--exit-after-frames", "--frames", "120"},
                                                                       {"--hidden", "--exit-after-frames"},
                                                                       {"--exit-after-frames", "--no-dialogs"}}) {
        INFO(arguments.front() << " (" << arguments.size() << " argument(s))");
        const auto options = parse(arguments);
        REQUIRE(options.has_value());
        CHECK(options->noDialogs);
    }
}

TEST_CASE("app/command_line: window options", "[app][command_line]") {
    const auto options =
        parse({"--width", "1920", "--height=1080", "--fullscreen", "--hidden", "--no-vsync", "--smoke"});
    REQUIRE(options.has_value());
    CHECK(options->windowWidth == 1920);
    CHECK(options->windowHeight == 1080);
    CHECK(options->fullscreen);
    CHECK(options->hidden);
    CHECK_FALSE(options->vsync);

    const auto smallest = parse({"--width", "64", "--height", "64"});
    REQUIRE(smallest.has_value());
    CHECK(smallest->windowWidth == CommandLineOptions::kMinWindowExtent);
    const auto largest = parse({"--width", "16384", "--height", "16384"});
    REQUIRE(largest.has_value());
    CHECK(largest->windowHeight == CommandLineOptions::kMaxWindowExtent);

    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"--width", "0"}, {"--width", "63"}, {"--height", "16385"}, {"--height", "-720"}}) {
        const auto rejected = parse(arguments);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::OutOfRange);
        CHECK(rejected.error().context == arguments[0] + "=" + arguments[1]);
    }
    const auto malformed = parse({"--width", "wide"});
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == ErrorCode::ParseError);
}

TEST_CASE("app/command_line: a hidden window needs a frame limit", "[app][command_line]") {
    const auto unlimited = parse({"--hidden"});
    REQUIRE_FALSE(unlimited.has_value());
    CHECK(unlimited.error().code == ErrorCode::InvalidArgument);
    CHECK(unlimited.error().context == "--hidden");

    const auto smoke = parse({"--hidden", "--smoke"});
    REQUIRE(smoke.has_value());
    CHECK(smoke->hidden);
    const auto limited = parse({"--hidden", "--exit-after-frames", "--frames", "10"});
    REQUIRE(limited.has_value());
    CHECK(limited->hidden);
    CHECK(limited->frameLimit == 10u);
}

TEST_CASE("app/command_line: log levels", "[app][command_line]") {
    const std::vector<std::pair<std::string, spdlog::level::level_enum>> accepted{
        {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug}, {"info", spdlog::level::info},
        {"warn", spdlog::level::warn},   {"error", spdlog::level::err},   {"WARN", spdlog::level::warn},
    };
    for (const auto& [text, level] : accepted) {
        const auto options = parse({"--log-level", text});
        REQUIRE(options.has_value());
        CHECK(options->logLevel == level);
    }
    for (const char* text : {"verbose", "warning", "critical", ""}) {
        const auto options = parse({"--log-level", text});
        REQUIRE_FALSE(options.has_value());
        CHECK(options.error().code == ErrorCode::InvalidArgument);
        CHECK(options.error().context == std::string("--log-level=") + text);
    }
}

TEST_CASE("app/command_line: directories are UTF-8 paths", "[app][command_line]") {
    const auto options =
        parse({"--asset-root", "D:/Giochi/Città/assets", "--data-dir=E:/Dati Retroverse", "--portable"});
    REQUIRE(options.has_value());
    REQUIRE(options->assetRoot.has_value());
    CHECK(*options->assetRoot == rv::core::pathFromUtf8("D:/Giochi/Città/assets"));
    CHECK(rv::core::pathToUtf8(*options->assetRoot) == "D:/Giochi/Città/assets");
    REQUIRE(options->dataDir.has_value());
    CHECK(*options->dataDir == rv::core::pathFromUtf8("E:/Dati Retroverse"));
    CHECK(options->portable);

    for (const char* option : {"--asset-root=", "--data-dir="}) {
        const auto empty = parse({option});
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("app/command_line: options of later steps are parsed and kept", "[app][command_line]") {
    const auto options = parse({"--scene", "m0_testroom", "--screenshot", "shots/G01.png", "--model", "crt.glb"});
    REQUIRE(options.has_value());
    CHECK(options->scene == "m0_testroom");
    CHECK(options->screenshot == rv::core::pathFromUtf8("shots/G01.png"));
    CHECK(options->model == rv::core::pathFromUtf8("crt.glb"));

    const auto emptyScene = parse({"--scene="});
    REQUIRE_FALSE(emptyScene.has_value());
    CHECK(emptyScene.error().context == "--scene");
}

TEST_CASE("app/command_line: --help and --version skip the remaining validation", "[app][command_line]") {
    for (const char* help : {"--help", "-h"}) {
        const auto options = parse({help});
        REQUIRE(options.has_value());
        CHECK(options->showHelp);
    }
    const auto version = parse({"--version", "--frames", "0"});
    REQUIRE(version.has_value());
    CHECK(version->showVersion);
    CHECK_FALSE(version->frameLimit.has_value());
}

TEST_CASE("app/command_line: unknown options and flag values are rejected", "[app][command_line]") {
    const auto unknown = parse({"--smoke", "--bogus"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::InvalidArgument);
    CHECK(unknown.error().context == "--bogus");

    CHECK_FALSE(parse({"--smoke=yes"}).has_value());
    CHECK_FALSE(parse({"--width"}).has_value()); // missing value
    CHECK_FALSE(parse({"m0_testroom"}).has_value()); // no positional arguments
}

TEST_CASE("app/command_line: argc/argv overload skips the program name", "[app][command_line]") {
    const char* const argv[] = {"retroverse.exe", "--smoke", "--frames", "4", "--hidden"};
    const auto options = rv::app::parseCommandLine(5, argv);
    REQUIRE(options.has_value());
    CHECK(options->smoke);
    CHECK(options->hidden);
    CHECK(options->frameLimit == 4u);
}

TEST_CASE("app/command_line: help lists every option and the exit codes", "[app][command_line]") {
    const std::string help = rv::app::commandLineHelp();
    for (const char* option :
         {"--version", "--width", "--height", "--fullscreen", "--hidden", "--no-vsync", "--smoke", "--frames",
          "--exit-after-frames", "--exit", "--gl-selftest", "--asset-root", "--data-dir", "--portable", "--log-level",
          "--no-dialogs", "--scene", "--screenshot", "--model"}) {
        INFO(option);
        CHECK(helpListsOption(help, option));
    }
    CHECK_FALSE(helpListsOption(help, "--exi")); // a prefix of a listed option is not an entry
    CHECK(help.find("  --exit\n      Same as --exit-after-frames.") != std::string::npos);
    CHECK(help.find("Exit codes") != std::string::npos);
}
