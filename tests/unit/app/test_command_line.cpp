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
    CHECK_FALSE(options->debugDraw);
    CHECK_FALSE(options->selfCheck.has_value());
    CHECK_FALSE(options->devUiOpen);
    CHECK_FALSE(options->screenshotIncludeUi);
    CHECK_FALSE(options->spawn.has_value());
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

TEST_CASE("app/command_line: --spawn places the walker", "[app][command_line][spawn]") {
    // Feet only: the scene camera's yaw and pitch.
    const auto feet = parse({"--spawn", "1.5,0,-3.25"});
    REQUIRE(feet.has_value());
    REQUIRE(feet->spawn.has_value());
    CHECK(feet->spawn->feet == glm::vec3(1.5f, 0.0f, -3.25f));
    CHECK_FALSE(feet->spawn->yawDeg.has_value());
    CHECK_FALSE(feet->spawn->pitchDeg.has_value());

    // With a yaw, then a yaw and a pitch; negative values and spaces around them; any yaw (it wraps).
    const auto turned = parse({"--spawn", "-1.5, 0 ,-3.8,90"});
    REQUIRE(turned.has_value());
    CHECK(turned->spawn->feet == glm::vec3(-1.5f, 0.0f, -3.8f));
    CHECK(turned->spawn->yawDeg == 90.0f);
    CHECK_FALSE(turned->spawn->pitchDeg.has_value());
    const auto full = parse({"--spawn=0,0,3.2,-720,-6", "--exit-after-frames"});
    REQUIRE(full.has_value());
    CHECK(full->spawn == rv::app::SpawnPoint{glm::vec3(0.0f, 0.0f, 3.2f), -720.0f, -6.0f});
    CHECK(rv::app::parseSpawnPoint("0,0,0,0,89").has_value());
    CHECK(rv::app::parseSpawnPoint("1e2,2.5e-1,-0").has_value());

    struct Case {
        const char* text;
        ErrorCode code;
    };
    for (const Case& c : {Case{"", ErrorCode::ParseError}, Case{"1,2", ErrorCode::ParseError},
                          Case{"1,2,3,4,5,6", ErrorCode::ParseError}, Case{"1,,3", ErrorCode::ParseError},
                          Case{"1,2,three", ErrorCode::ParseError}, Case{"1,2,3x", ErrorCode::ParseError},
                          Case{"+1,2,3", ErrorCode::ParseError}, Case{"1;2;3", ErrorCode::ParseError},
                          Case{"nan,0,0", ErrorCode::OutOfRange}, Case{"0,0,inf", ErrorCode::OutOfRange},
                          Case{"1e6,0,0", ErrorCode::OutOfRange}, Case{"0,0,0,0,90", ErrorCode::OutOfRange},
                          Case{"0,0,0,0,-89.5", ErrorCode::OutOfRange}, Case{"0,0,0,1e40", ErrorCode::OutOfRange}}) {
        INFO(c.text);
        const auto rejected = parse({"--spawn", c.text});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == c.code);
        CHECK(rejected.error().context == std::string("--spawn=") + c.text);
    }
    // The value is required.
    CHECK_FALSE(parse({"--spawn"}).has_value());
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
                                "--frames", "5", "--screenshot-size", "1280x720"});
    REQUIRE(options.has_value());
    CHECK(options->scene == "material_test");
    CHECK(options->screenshot == rv::core::pathFromUtf8("shots/G01.png"));
    CHECK(options->screenshotWidth == 1280);
    CHECK(options->screenshotHeight == 720);
    CHECK_FALSE(options->model.has_value());

    const auto emptyScene = parse({"--scene="});
    REQUIRE_FALSE(emptyScene.has_value());
    CHECK(emptyScene.error().context == "--scene");

    // A scene file instead of a name: any path is accepted as given.
    const auto file = parse({"--scene", "D:/Scene di prova/città.json"});
    REQUIRE(file.has_value());
    CHECK(file->scene == "D:/Scene di prova/città.json");
}

TEST_CASE("app/command_line: --model opens a glTF file in the model viewer instead of a scene", "[app][command_line]") {
    const auto options = parse({"--model", "build/models/Orientation Cube.GLTF", "--debug-draw"});
    REQUIRE(options.has_value());
    CHECK(options->model == rv::core::pathFromUtf8("build/models/Orientation Cube.GLTF"));
    CHECK(options->debugDraw);
    CHECK_FALSE(options->scene.has_value());
    CHECK(parse({"--model", "D:/modelli/città.glb"}).has_value());

    // Only glTF files.
    const auto obj = parse({"--model", "crt.obj"});
    REQUIRE_FALSE(obj.has_value());
    CHECK(obj.error().code == ErrorCode::InvalidArgument);
    CHECK(obj.error().context == "--model=crt.obj");
    CHECK_FALSE(parse({"--model", "crt"}).has_value());
    CHECK_FALSE(parse({"--model="}).has_value());

    // The viewer replaces the scene: both together are refused.
    const auto both = parse({"--scene", "m0_testroom", "--model", "crt.glb"});
    REQUIRE_FALSE(both.has_value());
    CHECK(both.error().code == ErrorCode::InvalidArgument);
    CHECK(both.error().context == "--model");
}

TEST_CASE("app/command_line: --debug-draw draws debug lines over any scene", "[app][command_line]") {
    const auto options = parse({"--scene", "m0_testroom", "--debug-draw", "--smoke"});
    REQUIRE(options.has_value());
    CHECK(options->debugDraw);
    CHECK_FALSE(parse({"--debug-draw=yes"}).has_value()); // a flag takes no value
}

TEST_CASE("app/command_line: --devui-open and --screenshot-include-ui show the developer overlay",
          "[app][command_line]") {
    const auto automated = parse({"--scene", "m0_testroom", "--devui-open", "--exit-after-frames", "--frames", "30",
                                  "--screenshot", "ui.png", "--screenshot-include-ui"});
    REQUIRE(automated.has_value());
    CHECK(automated->devUiOpen);
    CHECK(automated->screenshotIncludeUi);

    // Interactive runs may open the overlay too, and F12 screenshots honor --screenshot-include-ui.
    const auto interactive = parse({"--devui-open", "--screenshot-include-ui"});
    REQUIRE(interactive.has_value());
    CHECK(interactive->devUiOpen);
    CHECK(interactive->screenshotIncludeUi);
    const auto sceneOnly = parse({"--devui-open"});
    REQUIRE(sceneOnly.has_value());
    CHECK_FALSE(sceneOnly->screenshotIncludeUi);

    CHECK_FALSE(parse({"--devui-open=1"}).has_value()); // flags take no value
    CHECK_FALSE(parse({"--screenshot-include-ui=yes"}).has_value());
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

TEST_CASE("app/command_line: --self-check runs a named check in a frame-limited run", "[app][command_line]") {
    for (const char* limit : {"--smoke", "--exit-after-frames"}) {
        INFO(limit);
        const auto options = parse({"--self-check", "srgb", limit, "--hidden"});
        REQUIRE(options.has_value());
        CHECK(options->selfCheck == rv::app::SelfCheck::Srgb);
        const auto bloom = parse({"--self-check", "bloom", limit, "--hidden"});
        REQUIRE(bloom.has_value());
        CHECK(bloom->selfCheck == rv::app::SelfCheck::Bloom);
        const auto fxaa = parse({"--self-check", "fxaa", limit, "--hidden"});
        REQUIRE(fxaa.has_value());
        CHECK(fxaa->selfCheck == rv::app::SelfCheck::Fxaa);
    }

    // An interactive run has no end for the check to report to.
    const auto interactive = parse({"--self-check", "srgb"});
    REQUIRE_FALSE(interactive.has_value());
    CHECK(interactive.error().code == ErrorCode::InvalidArgument);
    CHECK(interactive.error().context == "--self-check");

    for (const char* name : {"SRGB", "Bloom", "FXAA", "gamma", ""}) {
        INFO(name);
        const auto rejected = parse({"--exit-after-frames", "--self-check", name});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::InvalidArgument);
        CHECK(rejected.error().context == std::string("--self-check=") + name);
    }
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
                               "--input-script", "--fixed-dt",   "--self-check", "--debug-draw"}) {
        INFO(option);
        CHECK(helpListsOption(help, option));
    }
    CHECK(helpListsOption(help, "--devui-open"));
    CHECK(helpListsOption(help, "--screenshot-include-ui"));
    CHECK(helpListsOption(help, "--spawn"));
    CHECK(helpListsOption(help, "--run-report"));
    CHECK_FALSE(helpListsOption(help, "--exi")); // a prefix of a listed option is not an entry
    CHECK(help.find("  --exit\n      Same as --exit-after-frames.") != std::string::npos);
    CHECK(help.find("Exit codes") != std::string::npos);
}

TEST_CASE("app/command_line: the options of I3 are parsed, listed and no longer refused",
          "[app][command_line]") {
    // A valid use of each option is an invalid command line (exit 2) that names the step implementing it.
    const std::vector<std::pair<std::vector<std::string>, std::string>> pending{
        // None since I3 steps 11 (--video-freeze-pts) and 13r (--refresh-library).
    };
    for (const auto& [arguments, step] : pending) {
        INFO(arguments.back());
        const auto refused = parse(arguments);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::Unsupported);
        CHECK(refused.error().message == "not implemented yet (" + step + ")");
    }
    // I3 step 13r: --refresh-library is taken.
    const auto refresh = parse({"--refresh-library"});
    REQUIRE(refresh.has_value());
    CHECK(refresh->refreshLibrary);
    CHECK_FALSE(parse({"--smoke"})->refreshLibrary);
    // I3 step 11: --video-freeze-pts is taken with a frame limit.
    const auto frozen = parse({"--smoke", "--video-freeze-pts", "2.5"});
    REQUIRE(frozen.has_value());
    CHECK(frozen->videoFreezePtsSeconds == 2.5);

    // Their values are validated before the refusal.
    const auto interactive = parse({"--video-freeze-pts", "1"});
    REQUIRE_FALSE(interactive.has_value());
    CHECK(interactive.error().code == ErrorCode::InvalidArgument);
    CHECK(interactive.error().context == "--video-freeze-pts");
    for (const char* value : {"-1", "86401", "nan", "inf"}) {
        INFO(value);
        const auto outOfRange = parse({"--smoke", "--video-freeze-pts", value});
        REQUIRE_FALSE(outOfRange.has_value());
        CHECK(outOfRange.error().code == ErrorCode::OutOfRange);
    }
    const auto notANumber = parse({"--smoke", "--video-freeze-pts", "soon"});
    REQUIRE_FALSE(notANumber.has_value());
    CHECK(notANumber.error().code == ErrorCode::ParseError);
    const auto flagValue = parse({"--refresh-library=yes"});
    REQUIRE_FALSE(flagValue.has_value());
    CHECK(flagValue.error().code == ErrorCode::InvalidArgument);

    // The refusal itself: nothing set, nothing refused.
    CHECK(rv::app::pendingOptionError(CommandLineOptions{}).has_value());
    CommandLineOptions options;
    options.refreshLibrary = true;
    CHECK(rv::app::pendingOptionError(options).has_value()); // taken since I3 step 13r
    options.videoFreezePtsSeconds = 1.0;
    CHECK(rv::app::pendingOptionError(options).has_value()); // taken since I3 step 11

    const std::string help = rv::app::commandLineHelp();
    CHECK(helpListsOption(help, "--refresh-library"));
    CHECK(helpListsOption(help, "--video-freeze-pts"));
}

TEST_CASE("app/command_line: --video-freeze-pts holds the hero CRT's frame of a frame-limited run (I3 step 11)",
          "[app][command_line]") {
    CHECK_FALSE(parse({"--smoke"})->videoFreezePtsSeconds.has_value());
    const auto frozen = parse({"--exit-after-frames", "--video-freeze-pts", "1.0"});
    REQUIRE(frozen.has_value());
    CHECK(frozen->videoFreezePtsSeconds == 1.0);
    const auto start = parse({"--smoke", "--video-freeze-pts", "0"});
    REQUIRE(start.has_value());
    CHECK(start->videoFreezePtsSeconds == 0.0);
    const auto longest = parse({"--smoke", "--video-freeze-pts", "86400"});
    REQUIRE(longest.has_value());
    CHECK(longest->videoFreezePtsSeconds == CommandLineOptions::kMaxVideoFreezePtsSeconds);
    CommandLineOptions options;
    options.videoFreezePtsSeconds = 2.5;
    CHECK(rv::app::pendingOptionError(options).has_value());
    CHECK(rv::app::commandLineHelp().find("Not implemented yet (I3 step 11)") == std::string::npos);
}

TEST_CASE("app/command_line: --no-texture-compression keeps the collection's arrays uncompressed (I3 step 6)",
          "[app][command_line]") {
    const auto plain = parse({});
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->noTextureCompression);
    const auto interactive = parse({"--no-texture-compression"});
    REQUIRE(interactive.has_value());
    CHECK(interactive->noTextureCompression);
    const auto limited = parse({"--smoke", "--frames", "5", "--no-texture-compression"});
    REQUIRE(limited.has_value());
    CHECK(limited->noTextureCompression);
    CommandLineOptions options;
    options.noTextureCompression = true;
    CHECK(rv::app::pendingOptionError(options).has_value());
    const auto flagValue = parse({"--no-texture-compression=yes"});
    REQUIRE_FALSE(flagValue.has_value());
    CHECK(flagValue.error().code == ErrorCode::InvalidArgument);
    const std::string help = rv::app::commandLineHelp();
    CHECK(helpListsOption(help, "--no-texture-compression"));
    CHECK(help.find("Not implemented yet (I3 step 6)") == std::string::npos);
}
