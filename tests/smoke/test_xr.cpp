// The VR mode (I10 step 2, ADR-0017 §3, §10) with the fake runtime: no headset, two asymmetric eyes at the input
// script's head poses, a steady 72 Hz with no wait. The session paces the run, the eyes go into the swapchain's
// images, the screenshot shows both eyes side by side, the window the left eye; a lost session sends the run back to
// the desktop, and a missing runtime keeps it there. I10 step 3: the stick walks along the head, the right stick snaps a
// turn and aims the teleport, xr.json seats the user. Golden G21: both eyes in the test room from the scene camera's
// place (compared on the reference machine only, like G01; RV_UPDATE_GOLDEN=1 rewrites it from this run, then fails).
//   $env:RV_UPDATE_GOLDEN=1; build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden/G21:*"
//   Remove-Item Env:RV_UPDATE_GOLDEN

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/screenshot_diff/screenshot_diff.hpp>

#include "smoke_support.hpp"

using Catch::Approx;
using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

/// The fake runtime's eyes (xr::FakeRuntimeConfig's recommendation at render scale 1): the screenshot holds both.
constexpr glm::ivec2 kEyeSize(1024, 1024);
/// The test room's scene camera, where the walker stands: feet (-0.6, 0, 0.9) m, yaw 9 degrees.
constexpr glm::vec3 kFeet(-0.6f, 0.0f, 0.9f);
constexpr float kWalkerYawDeg = 9.0f;

std::vector<std::string> vrArguments(int frames, const fs::path& shot) {
    return {"--scene",    "m0_testroom",       "--vr",          "--xr-runtime", "fake",   "--exit-after-frames",
            "--frames",   std::to_string(frames), "--fixed-dt", "0.016667",     "--screenshot", arg(shot)};
}

const rv::app::XrHeadSample* headAt(const rv::app::XrRecord& xr, std::uint64_t frame) {
    for (const rv::app::XrHeadSample& sample : xr.head) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN"); // NOLINT(concurrency-mt-unsafe): read before any thread
    return value != nullptr && std::string_view(value) == "1";
}

std::string fileSha256(const fs::path& file) {
    auto bytes = rv::core::readBinaryFile(file);
    REQUIRE(bytes.has_value());
    return rv::core::sha256Hex(std::span<const std::byte>(*bytes));
}

} // namespace

TEST_CASE("xr: the fake runtime paces the test room, each eye into its image, the head as the script turns it "
          "(I10 step 2)",
          "[gpu][xr]") {
    const fs::path dir = freshCaseDir("xr/look");
    const fs::path shot = dir / "eyes.png";
    std::vector<std::string> arguments = vrArguments(40, shot);
    arguments.insert(arguments.end(), {"--input-script", arg(sourceDir() / "tests/input_scripts/xr_look.json")});
    const AppRun run = runApp(retroverseExe(), dir, "look", arguments);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    // Both eyes side by side, each at the runtime's size.
    requireScreenshot(run, shot, glm::ivec2(2 * kEyeSize.x, kEyeSize.y));

    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.runtimeChoice == "fake");
    CHECK(xr.started);
    CHECK_FALSE(xr.unavailable);
    CHECK(xr.space == "local-floor");
    CHECK(xr.eyeSize == kEyeSize);
    CHECK(xr.format == "GL_SRGB8_ALPHA8");
    CHECK(xr.srgb);
    CHECK(xr.reachedFocused);
    CHECK(xr.endReason.empty());
    CHECK_FALSE(xr.desktopFromFrame);
    // Every frame waited for, located and submitted with both eyes, none late at the fake's steady 72 Hz.
    CHECK(xr.framesWaited == 40);
    CHECK(xr.framesWithViews == 40);
    CHECK(xr.refreshHz == Approx(72.0).margin(0.01));
    CHECK(xr.missedRefreshes == 0);
    CHECK(xr.events == std::vector<std::string>{"0: idle", "0: ready", "0: synchronized", "0: visible", "0: focused"});

    // The head follows the script, its yaw on top of the walker's, its height from tracking (LOCAL_FLOOR on the
    // walker's feet).
    REQUIRE(xr.head.size() == 40);
    struct Expected {
        std::uint64_t frame;
        float yawDeg;
        float pitchDeg;
        float rollDeg;
        glm::vec3 position;
    };
    const float yaw = rv::core::degreesToRadians(kWalkerYawDeg);
    // 0.3 m ahead of the walker, who faces (sin yaw, 0, -cos yaw).
    const glm::vec3 stepped = kFeet + glm::vec3(0.3f * std::sin(yaw), 1.2f, -0.3f * std::cos(yaw));
    const glm::vec3 standing = kFeet + glm::vec3(0.0f, 1.65f, 0.0f);
    for (const Expected& expected : {Expected{0, kWalkerYawDeg, 0.0f, 0.0f, standing},
                                     Expected{15, kWalkerYawDeg - 40.0f, 0.0f, 0.0f, standing},
                                     Expected{25, kWalkerYawDeg + 35.0f, -20.0f, 0.0f, standing},
                                     Expected{39, kWalkerYawDeg, 0.0f, 10.0f, stepped}}) {
        INFO("frame " << expected.frame);
        const rv::app::XrHeadSample* sample = headAt(xr, expected.frame);
        REQUIRE(sample != nullptr);
        CHECK(sample->yawDeg == Approx(expected.yawDeg).margin(0.01));
        CHECK(sample->pitchDeg == Approx(expected.pitchDeg).margin(0.01));
        CHECK(sample->rollDeg == Approx(expected.rollDeg).margin(0.01));
        CHECK(sample->position.x == Approx(expected.position.x).margin(1e-4));
        CHECK(sample->position.y == Approx(expected.position.y).margin(1e-4));
        CHECK(sample->position.z == Approx(expected.position.z).margin(1e-4));
    }
}

TEST_CASE("xr: the stick walks along the head, snaps a turn, and the teleport lands where its arc came down "
          "(I10 step 3)",
          "[gpu][xr]") {
    const fs::path dir = freshCaseDir("xr/walk");
    const fs::path shot = dir / "eyes.png";
    std::vector<std::string> arguments = vrArguments(70, shot);
    arguments.insert(arguments.end(), {"--spawn", "0,0,3.2,0,0", "--input-script",
                                       arg(sourceDir() / "tests/input_scripts/xr_walk.json")});
    const AppRun run = runApp(retroverseExe(), dir, "walk", arguments);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.framesWithViews == 70);
    CHECK(xr.posture == "standing");
    CHECK(xr.turn == "snap");
    CHECK(xr.snapTurns == 1);
    CHECK(xr.teleports == 1);
    CHECK(xr.pushedFrames == 0);

    // The head turned west in the space: the stick walked it 1.25 m west, not north where the walker faced.
    const rv::app::XrHeadSample* walked = headAt(xr, 39);
    REQUIRE(walked != nullptr);
    CHECK(walked->position.x == Approx(-1.25f).margin(0.01));
    CHECK(walked->position.z == Approx(3.2f).margin(0.01));
    CHECK(walked->yawDeg == Approx(-90.0f).margin(0.01));
    // The snap turn turned the head 30 degrees right about itself.
    const rv::app::XrHeadSample* turned = headAt(xr, 45);
    REQUIRE(turned != nullptr);
    CHECK(turned->yawDeg == Approx(-60.0f).margin(0.01));
    CHECK(turned->position.x == Approx(walked->position.x).margin(1e-4));
    CHECK(turned->position.z == Approx(walked->position.z).margin(1e-4));
    // Let go at frame 60: the head jumped about 1.73 m along its heading, 60 degrees left of north.
    const rv::app::XrHeadSample* aimed = headAt(xr, 59);
    const rv::app::XrHeadSample* landed = headAt(xr, 61);
    REQUIRE(aimed != nullptr);
    REQUIRE(landed != nullptr);
    const glm::vec2 jump(landed->position.x - aimed->position.x, landed->position.z - aimed->position.z);
    CHECK(glm::length(jump) == Approx(1.73f).margin(0.05));
    const glm::vec2 heading(std::sin(rv::core::degreesToRadians(-60.0f)), -std::cos(rv::core::degreesToRadians(-60.0f)));
    CHECK(glm::dot(glm::normalize(jump), heading) > 0.999f);
    CHECK(landed->position.y == Approx(1.65f).margin(1e-4));
}

TEST_CASE("xr: xr.json seats the user at the walker's eye height, the comfort vignette on (I10 step 3)", "[gpu][xr]") {
    const fs::path dir = freshCaseDir("xr/seated");
    fs::create_directories(dir / "data" / "config");
    REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "config" / "xr.json",
                                          "{\"posture\": \"seated\", \"comfort\": {\"turn\": \"smooth\", "
                                          "\"vignette\": 0.8}}\n")
                .has_value());
    const fs::path script = dir / "seated.json";
    // Seated 1.1 m up, the stick forward for the second half of the run.
    REQUIRE(rv::core::writeTextFileAtomic(script, R"([
  {"fromFrame": 0, "toFrame": 9, "headPose": {"position": [0, 1.1, 0]}},
  {"fromFrame": 10, "toFrame": 19, "headPose": {"position": [0, 1.1, 0]}, "actions": {"move_forward": 1.0}}
])")
                .has_value());
    const fs::path shot = dir / "eyes.png";
    std::vector<std::string> arguments = vrArguments(20, shot);
    arguments.insert(arguments.end(), {"--input-script", arg(script)});
    const AppRun run = runApp(retroverseExe(), dir, "seated", arguments);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    requireScreenshot(run, shot, glm::ivec2(2 * kEyeSize.x, kEyeSize.y));
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.posture == "seated");
    CHECK(xr.turn == "smooth");
    for (const std::uint64_t frame : {0u, 19u}) {
        const rv::app::XrHeadSample* sample = headAt(xr, frame);
        REQUIRE(sample != nullptr);
        CHECK(sample->position.y == Approx(1.65f).margin(1e-4));
    }
}

TEST_CASE("xr: a lost session sends the run back to the desktop, which renders on (I10 step 2)", "[gpu][xr]") {
    const fs::path dir = freshCaseDir("xr/lost");
    const fs::path shot = dir / "desktop.png";
    std::vector<std::string> arguments = vrArguments(20, shot);
    arguments.insert(arguments.end(), {"--xr-fake-lose-after", "10"});
    const AppRun run = runApp(retroverseExe(), dir, "lost", arguments);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    // The last frame is the desktop's: its screenshot at --screenshot-size.
    requireScreenshot(run, shot, glm::ivec2(1920, 1080));
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.started);
    CHECK(xr.endReason == "lost");
    CHECK(xr.desktopFromFrame == std::optional<std::uint64_t>(10));
    CHECK(xr.framesWaited == 10);
    CHECK(xr.framesWithViews == 10);
    CHECK(xr.head.size() == 10);
    CHECK(run.report->framesRendered == 20);
}

TEST_CASE("xr: without a runtime the run stays on the desktop and says why (I10 step 2)", "[gpu][xr]") {
    const fs::path dir = freshCaseDir("xr/no_runtime");
    const fs::path shot = dir / "desktop.png";
    // A manifest that does not exist: the loader finds no runtime, and the system's runtime is never asked.
    const AppRun run = runApp(retroverseExe(), dir, "no_runtime",
                              {"--scene", "m0_testroom", "--vr", "--xr-runtime-json", arg(dir / "missing_runtime.json"),
                               "--exit-after-frames", "--frames", "3", "--screenshot", arg(shot)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    requireScreenshot(run, shot, glm::ivec2(1920, 1080));
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.runtimeChoice == "openxr");
    CHECK_FALSE(xr.started);
    REQUIRE(xr.unavailable);
    CHECK_FALSE(xr.unavailable->empty());
    CHECK(xr.framesWaited == 0);
    CHECK(xr.head.empty());
}

TEST_CASE("golden/G21: the test room in the fake headset, both eyes from the scene camera's place (I10 step 2)",
          "[gpu][golden][xr]") {
    const fs::path goldenDir = sourceDir() / "assets/style_tests/screenshots";
    const fs::path golden = goldenDir / "g21_xr_testroom_golden.png";
    const fs::path metadataFile = goldenDir / "g21_xr_testroom_golden.json";
    constexpr int kFrames = 30;
    const fs::path dir = freshCaseDir("golden_g21");
    const fs::path shot = dir / "g21.png";
    const AppRun run = runApp(retroverseExe(), dir, "g21", vrArguments(kFrames, shot));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const glm::ivec2 size(2 * kEyeSize.x, kEyeSize.y);
    requireScreenshot(run, shot, size);
    REQUIRE(run.report->xr);
    REQUIRE(run.report->xr->framesWithViews == kFrames);
    const fs::path verdictFile = dir / "comparison.txt";
    const rv::app::RunReportContext& context = *run.report->context;

    if (updatingGoldens()) {
        std::error_code error;
        fs::copy_file(shot, golden, fs::copy_options::overwrite_existing, error);
        INFO(error.message());
        REQUIRE_FALSE(error);
        const std::string preset = rv::core::pathToUtf8(retroverseExe().parent_path().parent_path().filename());
        const nlohmann::json metadata{
            {"schemaVersion", 1},
            {"id", "G21"},
            {"description", "I10 step 2's golden of the VR mode: the M0 test room in the fake runtime's headset, both "
                            "eyes side by side (left, right), each an off-axis view of a Quest 3S's field 64 mm "
                            "apart, the head standing 1.65 m above the scene camera's feet and facing its yaw; the "
                            "last frame of a 30-frame run."},
            {"image",
             {{"file", "g21_xr_testroom_golden.png"},
              {"width", size.x},
              {"height", size.y},
              {"format", "PNG, 8-bit RGB, sRGB-encoded as the swapchain images hold it"},
              {"sha256", fileSha256(golden)}}},
            {"scene",
             {{"file", "assets/style_tests/reference_scenes/m0_testroom.json"},
              {"sha256", fileSha256(sourceDir() / "assets/style_tests/reference_scenes/m0_testroom.json")}}},
            {"run",
             {{"frames", kFrames},
              {"capturedFrame", kFrames - 1},
              {"command", "retroverse.exe --scene m0_testroom --vr --xr-runtime fake --exit-after-frames --frames 30 "
                          "--fixed-dt 0.016667 --hidden --screenshot <file>"},
              {"regenerate", "$env:RV_UPDATE_GOLDEN=1; build/" + preset +
                                 "/bin/rv_smoke_tests.exe \"golden/G21:*\"; Remove-Item Env:RV_UPDATE_GOLDEN (then "
                                 "run it again without the variable; tests/smoke/test_xr.cpp)"}}},
            {"build",
             {{"preset", preset},
              {"buildType", run.report->buildType},
              {"sources", "the commit that adds or rewrites this metadata, rendered from its working tree before "
                          "committing"}}},
            {"gpu",
             {{"vendor", context.vendor},
              {"renderer", context.renderer},
              {"glVersion", context.version},
              {"machine", "docs/testing/reference-hardware.md"}}},
            {"renderedOn", rv::core::utcNow().substr(0, 10)},
            {"comparison", "Compared by tests/smoke/test_xr.cpp with G01's tolerance (6/255 per channel, at most 0.5% "
                           "of the pixels) on this GPU and driver only."}};
        REQUIRE(rv::core::writeTextFileAtomic(metadataFile, metadata.dump(2) + "\n").has_value());
        REQUIRE(rv::core::writeTextFileAtomic(verdictFile, "rewritten\n").has_value());
        FAIL("golden G21 rewritten from this run (" << rv::core::pathToUtf8(golden)
                                                    << "): look at it, then run again without RV_UPDATE_GOLDEN");
    }

    const auto metadata = rv::core::readJsonFile(metadataFile);
    INFO(rv::core::pathToUtf8(metadataFile));
    REQUIRE(metadata.has_value());
    const std::string renderer = metadata->at("gpu").at("renderer").get<std::string>();
    const std::string glVersion = metadata->at("gpu").at("glVersion").get<std::string>();
    if (context.renderer != renderer || context.version != glVersion) {
        const std::string reason = "golden G21 was rendered on " + renderer + " (" + glVersion +
                                   "), this machine has " + context.renderer + " (" + context.version +
                                   "): the comparison runs on the reference machine only "
                                   "(docs/testing/reference-hardware.md)";
        REQUIRE(rv::core::writeTextFileAtomic(verdictFile, "skipped: " + reason + "\n").has_value());
        SKIP(reason);
    }
    const rv::screenshot_diff::Tolerance tolerance;
    const fs::path heatmap = dir / "g21_heatmap.png";
    const auto comparison = rv::screenshot_diff::compareFiles(golden, shot, tolerance, heatmap);
    REQUIRE(comparison.has_value());
    const std::string verdict = comparison->describe(tolerance);
    REQUIRE(rv::core::writeTextFileAtomic(verdictFile, verdict + "\n").has_value());
    INFO(verdict);
    INFO("heatmap of the differences: " << rv::core::pathToUtf8(heatmap));
    CHECK(comparison->matches);
}
