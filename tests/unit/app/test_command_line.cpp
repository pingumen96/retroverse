#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
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
    CHECK(options->screenshotWidth == 1920);
    CHECK(options->screenshotHeight == 1080);
    CHECK_FALSE(options->inputScript.has_value());
    CHECK_FALSE(options->fixedDeltaSeconds.has_value());
    CHECK_FALSE(options->model.has_value());
}

TEST_CASE("app/command_line: --input-script and --fixed-dt drive automated runs", "[app][command_line]") {
    const auto options = parse({"--exit-after-frames", "--frames", "120", "--fixed-dt", "0.016667", "--input-script",
                                "tests/input_scripts/walk forward.json"});
    REQUIRE(options.has_value());
    CHECK(options->inputScript == rv::core::pathFromUtf8("tests/input_scripts/walk forward.json"));
    REQUIRE(options->fixedDeltaSeconds.has_value());
    CHECK(*options->fixedDeltaSeconds == 0.016667);

    // Neither needs a frame limit: a script may open an interactive session, a fixed step may pace one.
    const auto interactive = parse({"--input-script=script.json", "--fixed-dt=1e-3"});
    REQUIRE(interactive.has_value());
    CHECK(interactive->inputScript == rv::core::pathFromUtf8("script.json"));
    CHECK(interactive->fixedDeltaSeconds == 0.001);
    const auto largest = parse({"--fixed-dt", "1"});
    REQUIRE(largest.has_value());
    CHECK(largest->fixedDeltaSeconds == CommandLineOptions::kMaxFixedDeltaSeconds);

    const auto emptyScript = parse({"--input-script="});
    REQUIRE_FALSE(emptyScript.has_value());
    CHECK(emptyScript.error().code == ErrorCode::InvalidArgument);
    CHECK(emptyScript.error().context == "--input-script");

    for (const char* text : {"0", "-0.016", "1.5", "inf", "nan", "1e400"}) {
        INFO(text);
        const auto rejected = parse({"--fixed-dt", text});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().context.starts_with("--fixed-dt="));
    }
    for (const char* text : {"fast", "0.016s", "", "1/60"}) {
        INFO(text);
        const auto rejected = parse({"--fixed-dt", text});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::ParseError);
    }
    const auto outOfRange = parse({"--fixed-dt", "2"});
    REQUIRE_FALSE(outOfRange.has_value());
    CHECK(outOfRange.error().code == ErrorCode::OutOfRange);
    CHECK(outOfRange.error().context == "--fixed-dt=2");
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

TEST_CASE("app/command_line: scene and screenshot options", "[app][command_line]") {
    const auto options = parse({"--scene", "material_test", "--screenshot", "shots/G01.png", "--exit-after-frames",
                                "--frames", "5", "--screenshot-size", "1280x720", "--model", "crt.glb"});
    REQUIRE(options.has_value());
    CHECK(options->scene == "material_test");
    CHECK(options->screenshot == rv::core::pathFromUtf8("shots/G01.png"));
    CHECK(options->screenshotWidth == 1280);
    CHECK(options->screenshotHeight == 720);
    CHECK(options->model == rv::core::pathFromUtf8("crt.glb")); // kept for the glTF step

    const auto emptyScene = parse({"--scene="});
    REQUIRE_FALSE(emptyScene.has_value());
    CHECK(emptyScene.error().context == "--scene");

    // A scene file instead of a name: any path is accepted as given.
    const auto file = parse({"--scene", "D:/Scene di prova/città.json"});
    REQUIRE(file.has_value());
    CHECK(file->scene == "D:/Scene di prova/città.json");
}

TEST_CASE("app/command_line: --screenshot needs a frame-limited run and a .png file", "[app][command_line]") {
    const auto interactive = parse({"--screenshot", "shot.png"});
    REQUIRE_FALSE(interactive.has_value());
    CHECK(interactive.error().code == ErrorCode::InvalidArgument);
    CHECK(interactive.error().context == "--screenshot");

    for (const char* limit : {"--smoke", "--exit", "--exit-after-frames"}) {
        INFO(limit);
        const auto options = parse({limit, "--screenshot", "build/screenshots/shot.PNG"});
        REQUIRE(options.has_value());
        CHECK(options->screenshot == rv::core::pathFromUtf8("build/screenshots/shot.PNG"));
    }
    for (const char* name : {"shot.jpg", "shot", "png"}) {
        INFO(name);
        const auto rejected = parse({"--smoke", "--screenshot", name});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::InvalidArgument);
        CHECK(rejected.error().context == std::string("--screenshot=") + name);
    }
}

TEST_CASE("app/command_line: --screenshot-size takes WIDTHxHEIGHT", "[app][command_line]") {
    const std::vector<std::pair<std::string, std::pair<int, int>>> accepted{{"1920x1080", {1920, 1080}},
                                                                            {"640X480", {640, 480}},
                                                                            {"1x1", {1, 1}},
                                                                            {"16384x16384", {16384, 16384}},
                                                                            {"0320x0200", {320, 200}}};
    for (const auto& [text, size] : accepted) {
        INFO(text);
        const auto options = parse({"--screenshot-size", text}); // also for F12 in interactive runs
        REQUIRE(options.has_value());
        CHECK(options->screenshotWidth == size.first);
        CHECK(options->screenshotHeight == size.second);
    }
    for (const char* text : {"1920", "1920x", "x1080", "1920*1080", "1920 x 1080", "-1920x1080", "+1920x1080",
                             "1920x1080x2", "wide x tall", "99999999999x1"}) {
        INFO(text);
        const auto rejected = parse({"--screenshot-size", text});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::ParseError);
        CHECK(rejected.error().context == std::string("--screenshot-size=") + text);
    }
    for (const char* text : {"0x1080", "1920x0", "16385x100", "100x16385"}) {
        INFO(text);
        const auto rejected = parse({"--screenshot-size", text});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::OutOfRange);
    }
}

TEST_CASE("app/command_line: scene names are restricted, scene paths are not", "[app][command_line]") {
    for (const char* name : {"m0_testroom", "material_test", "instancing-stress", "room.v2", "Room2"}) {
        INFO(name);
        CHECK(rv::app::isSceneName(name));
        CHECK(parse({"--scene", name}).has_value());
    }
    for (const char* path :
         {"scenes/room.json", "scenes\\room", "C:room", "room.json", "ROOM.JSON", "../room", "./room"}) {
        INFO(path);
        CHECK_FALSE(rv::app::isSceneName(path));
        CHECK(parse({"--scene", path}).has_value());
    }
    for (const char* name : {"my room", "room*", ".hidden", "..", "città"}) {
        INFO(name);
        REQUIRE(rv::app::isSceneName(name));
        const auto rejected = parse({"--scene", name});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::InvalidArgument);
        CHECK(rejected.error().context == std::string("--scene=") + name);
    }
}

TEST_CASE("app/command_line: scene names resolve under the asset root", "[app][command_line]") {
    const std::filesystem::path assets = rv::core::pathFromUtf8("D:/Retroverse/assets");
    const std::filesystem::path referenceScenes = assets / "style_tests" / "reference_scenes";

    CHECK(rv::app::resolveScenePath(std::nullopt, assets) == referenceScenes / "m0_testroom.json");
    CHECK(rv::app::resolveScenePath(std::string("material_test"), assets) == referenceScenes / "material_test.json");
    CHECK(rv::app::resolveScenePath(std::string("room.v2"), assets) == referenceScenes / "room.v2.json");
    CHECK(rv::app::resolveScenePath(std::nullopt, assets).native() ==
          (referenceScenes / "m0_testroom.json").lexically_normal().native());
    // Paths are used as given, relative to the working directory.
    CHECK(rv::app::resolveScenePath(std::string("build/scenes/test.json"), assets) ==
          rv::core::pathFromUtf8("build/scenes/test.json"));
    CHECK(rv::app::resolveScenePath(std::string("E:/Scene di prova/città.json"), assets) ==
          rv::core::pathFromUtf8("E:/Scene di prova/città.json"));
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
    for (const char* option : {"--version",      "--width",      "--height",     "--fullscreen",        "--hidden",
                               "--no-vsync",     "--smoke",      "--frames",     "--exit-after-frames", "--exit",
                               "--gl-selftest",  "--asset-root", "--data-dir",   "--portable",          "--log-level",
                               "--no-dialogs",   "--scene",      "--screenshot", "--screenshot-size",   "--model",
                               "--input-script", "--fixed-dt"}) {
        INFO(option);
        CHECK(helpListsOption(help, option));
    }
    CHECK_FALSE(helpListsOption(help, "--exi")); // a prefix of a listed option is not an entry
    CHECK(help.find("  --exit\n      Same as --exit-after-frames.") != std::string::npos);
    CHECK(help.find("Exit codes") != std::string::npos);
}
