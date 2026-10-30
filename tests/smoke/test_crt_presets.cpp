// The CRT presets on the world's screens (I5 step 6, ADR-0016 §7): tests/smoke/scenes/crt_presets.json's four CRTs
// showing their colour bars, each given one preset by the case's video.json (rooms.crt_presets.screens.<id>.crtPreset),
// seen together (the screenshot to review) and each close up, where the probes read the luma of a column of the
// picture's middle bar row by row: the neutral tube's rows have no scanline ripple, the arcade's ripple is deeper than
// the consumer's, the monitor's fainter. The run reports say which preset each screen took and why (crtLooks); video.json's
// crtEffect off makes every screen neutral, vintage steps each one stronger; the user's crt_presets.json reaches the
// screens (the consumer's scanlines turned off flatten its tube's rows).

#include "image_checks.hpp"
#include "smoke_support.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/scene_description.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rv::smoke;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kOverviewSize{1920, 1080};
/// The close-ups: the picture about 1,150 rows tall, a scanline over about 4.8 pixels (crt.glsl draws the ripple whole
/// from 4 pixels a line on).
constexpr glm::ivec2 kCloseSize{2560, 1440};
constexpr float kCloseDistance = 0.64f;

/// The screens and the preset each is given, in the scene's order.
const std::vector<std::pair<std::string, std::string>> kScreens = {{"crt_neutral", "neutral"},
                                                                   {"crt_consumer", "consumer"},
                                                                   {"crt_arcade", "arcade"},
                                                                   {"crt_monitor", "monitor"}};

std::filesystem::path sceneFile() {
    return sourceDir() / "tests" / "smoke" / "scenes" / "crt_presets.json";
}

/// video.json in the case's data directory: each screen its preset, plus `extra` global keys.
void writeVideo(const std::filesystem::path& dir, Json extra = Json::object()) {
    Json screens = Json::object();
    for (const auto& [id, preset] : kScreens) {
        screens[id] = Json{{"crtPreset", preset}};
    }
    Json settings = std::move(extra);
    settings["rooms"] = Json{{"crt_presets", {{"screens", std::move(screens)}}}};
    const std::filesystem::path config = dir / "data" / "config";
    std::filesystem::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
}

/// The point of `id`'s screen at its texture coordinates (u right, v down: uv (0, 0) its top-left): the crt prefab's
/// ScreenSurface (its rectangle a little in front of its rim plane) at the CRT's position in the scene.
glm::vec3 screenPoint(const rv::scene::SceneDescription& scene, std::string_view id, float u, float v) {
    const rv::scene::SceneObject* object = scene.findObject(id);
    REQUIRE(object != nullptr);
    auto crt = rv::geometry::buildCrt(rv::geometry::CrtParams{});
    REQUIRE(crt.has_value());
    const rv::geometry::Socket* surface = rv::geometry::findSocket(crt->sockets, "ScreenSurface");
    REQUIRE(surface != nullptr);
    const rv::core::Aabb& rect = surface->bounds;
    const glm::vec3 local(rect.min.x + u * (rect.max.x - rect.min.x), rect.max.y - v * (rect.max.y - rect.min.y),
                          0.5f * rect.max.z);
    return glm::vec3(object->instanceTransforms().front() * surface->transform * glm::vec4(local, 1.0f));
}

/// The scene with its camera `kCloseDistance` in front of `id`'s screen's middle, facing it, written into the case's
/// directory (its name, the room's id, unchanged).
std::filesystem::path writeCloseUp(const std::filesystem::path& dir, const rv::scene::SceneDescription& scene,
                                   std::string_view id, rv::scene::CameraSetup& camera) {
    auto document = rv::core::readJsonFile(sceneFile());
    REQUIRE(document.has_value());
    const glm::vec3 middle = screenPoint(scene, id, 0.5f, 0.5f);
    camera = scene.camera;
    camera.position = middle + glm::vec3(0.0f, 0.0f, kCloseDistance);
    camera.yawDeg = 0.0f;
    camera.pitchDeg = 0.0f;
    (*document)["camera"]["position"] = Json::array({camera.position.x, camera.position.y, camera.position.z});
    (*document)["camera"]["yawDeg"] = 0.0;
    (*document)["camera"]["pitchDeg"] = 0.0;
    const std::filesystem::path file = dir / rv::core::pathFromUtf8("close_" + std::string(id) + ".json");
    REQUIRE(rv::core::writeTextFileAtomic(file, document->dump(2) + "\n").has_value());
    return file;
}

/// A frame-limited run of `scene` with `extra` arguments.
AppRun runScene(const std::filesystem::path& dir, std::string_view name, const std::filesystem::path& scene,
                std::vector<std::string> extra) {
    std::vector<std::string> arguments{"--scene", arg(scene), "--exit-after-frames", "--frames", "5",
                                       "--fixed-dt", "0.016667"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    return run;
}

/// The report's look of each screen, by screen.
std::map<std::string, rv::app::CrtLookRecord, std::less<>> looksOf(const AppRun& run) {
    REQUIRE(run.report->crtLooks.has_value());
    std::map<std::string, rv::app::CrtLookRecord, std::less<>> looks;
    for (const rv::app::CrtLookRecord& look : run.report->crtLooks->screens) {
        looks.emplace(look.screen, look);
    }
    return looks;
}

/// The scanline ripple of `id`'s picture: the luma of a 5-pixel-wide column down the middle of its middle bar (the
/// colour bars' green, uniform down its height but for the tube's effects) from 15% to 55% of the screen's height, row
/// by row; the mean difference between neighbouring rows over the mean luma. A vignette or a curvature changes the
/// luma slowly (a fraction of a percent from a row to the next); the scanlines of 240 lines over 1,150 rows change it
/// by a good share of their depth every two rows.
struct Ripple {
    double meanLuma = 0.0;
    double ripple = 0.0;
};

Ripple rippleOf(const rv::assets::ImageData& image, const rv::scene::SceneDescription& scene,
                const rv::scene::CameraSetup& camera, std::string_view id) {
    const std::optional<glm::ivec2> top = projectToPixel(camera, kCloseSize, screenPoint(scene, id, 0.5f, 0.15f));
    const std::optional<glm::ivec2> bottom = projectToPixel(camera, kCloseSize, screenPoint(scene, id, 0.5f, 0.55f));
    REQUIRE(top.has_value());
    REQUIRE(bottom.has_value());
    REQUIRE(bottom->y - top->y > 300);
    std::vector<double> rows;
    for (int y = top->y; y <= bottom->y; ++y) {
        const double t = static_cast<double>(y - top->y) / static_cast<double>(bottom->y - top->y);
        const int x = static_cast<int>(std::lround(top->x + t * (bottom->x - top->x)));
        double sum = 0.0;
        for (int dx = -2; dx <= 2; ++dx) {
            sum += lumaAt(image, x + dx, y);
        }
        rows.push_back(sum / 5.0);
    }
    Ripple result;
    double steps = 0.0;
    for (std::size_t row = 0; row < rows.size(); ++row) {
        result.meanLuma += rows[row] / static_cast<double>(rows.size());
        if (row > 0) {
            steps += std::abs(rows[row] - rows[row - 1]) / static_cast<double>(rows.size() - 1);
        }
    }
    result.ripple = result.meanLuma > 0.0 ? steps / result.meanLuma : 0.0;
    return result;
}

/// A close-up run of `id`'s screen (named `name`, with the case's video.json and crt_presets.json as they are) and the
/// ripple of its picture.
struct CloseUp {
    AppRun run;
    Ripple ripple;
};

CloseUp closeUp(const std::filesystem::path& dir, const rv::scene::SceneDescription& scene, std::string_view id,
                const std::string& name) {
    rv::scene::CameraSetup camera;
    const std::filesystem::path closeScene = writeCloseUp(dir, scene, id, camera);
    const std::filesystem::path shot = dir / rv::core::pathFromUtf8(name + ".png");
    CloseUp result{runScene(dir, name, closeScene, {"--screenshot", arg(shot), "--screenshot-size", "2560x1440"}), {}};
    INFO(result.run.describe());
    const rv::assets::ImageData image = requireScreenshot(result.run, shot, kCloseSize);
    result.ripple = rippleOf(image, scene, camera, id);
    WARN(name << ": mean luma " << result.ripple.meanLuma << ", ripple " << result.ripple.ripple);
    return result;
}

} // namespace

TEST_CASE("crt presets: each preset on its own tube, the neutral flat, the arcade deeper than the consumer",
          "[gpu][crt_presets]") {
    const std::filesystem::path dir = freshCaseDir("crt_presets/presets");
    writeVideo(dir);
    const auto scene = rv::scene::loadSceneDescription(sceneFile());
    REQUIRE(scene.has_value());

    // The four together (the screenshot to review); each screen's preset from video.json, the user's rule.
    const std::filesystem::path overview = dir / "overview.png";
    const AppRun all = runScene(dir, "overview", sceneFile(),
                                {"--screenshot", arg(overview), "--screenshot-size", "1920x1080"});
    INFO(all.describe());
    requireScreenshot(all, overview, kOverviewSize);
    REQUIRE(all.report->crtLooks.has_value());
    CHECK(all.report->crtLooks->crtEffect == "light");
    CHECK(all.report->crtLooks->presetsSource == "shipped");
    const auto looks = looksOf(all);
    REQUIRE(looks.size() == kScreens.size());
    for (const auto& [id, preset] : kScreens) {
        INFO(id);
        const auto look = looks.find(id);
        REQUIRE(look != looks.end());
        CHECK(look->second.room == "crt_presets");
        CHECK(look->second.kind == (id == "crt_neutral" ? "hero" : "secondary"));
        CHECK(look->second.preset == preset);
        CHECK(look->second.chosen == preset);
        CHECK(look->second.rule == "video.json");
    }

    // Each close up: the ripple of its rows.
    std::map<std::string, Ripple, std::less<>> ripples;
    for (const auto& [id, preset] : kScreens) {
        const Ripple ripple = closeUp(dir, *scene, id, "close_" + preset).ripple;
        CHECK(ripple.meanLuma > 40.0); // the bar is lit, not the tube's border
        ripples.emplace(preset, ripple);
    }
    // neutral: no row modulation (the clean video: only the picture's own slow change from a row to the next).
    CHECK(ripples.at("neutral").ripple < 0.005);
    // consumer: the scanlines show; arcade: deeper; monitor: finer and fainter than consumer, still a tube.
    CHECK(ripples.at("consumer").ripple > 0.02);
    CHECK(ripples.at("arcade").ripple > 1.25 * ripples.at("consumer").ripple);
    CHECK(ripples.at("monitor").ripple < 0.6 * ripples.at("consumer").ripple);
    // The scanlines keep the picture's mean brightness: the four bars' mean luma within 12% of each other (the arcade's
    // and the consumer's vignette and curvature aside).
    double brightest = 0.0;
    double darkest = 1e9;
    for (const auto& [preset, ripple] : ripples) {
        brightest = std::max(brightest, ripple.meanLuma);
        darkest = std::min(darkest, ripple.meanLuma);
    }
    CHECK(darkest > 0.88 * brightest);
}

TEST_CASE("crt presets: crtEffect off makes every screen neutral, vintage steps each one stronger",
          "[gpu][crt_presets]") {
    const std::filesystem::path dir = freshCaseDir("crt_presets/effect");
    const std::map<std::string, std::map<std::string, std::string>> expected = {
        {"off",
         {{"crt_neutral", "neutral"}, {"crt_consumer", "neutral"}, {"crt_arcade", "neutral"}, {"crt_monitor", "neutral"}}},
        {"vintage",
         {{"crt_neutral", "neutral"}, {"crt_consumer", "arcade"}, {"crt_arcade", "arcade"}, {"crt_monitor", "consumer"}}},
        {"light",
         {{"crt_neutral", "neutral"}, {"crt_consumer", "consumer"}, {"crt_arcade", "arcade"}, {"crt_monitor", "monitor"}}},
    };
    for (const auto& [effect, presets] : expected) {
        INFO("crtEffect " << effect);
        writeVideo(dir, Json{{"crtEffect", effect}});
        const AppRun run = runScene(dir, "effect_" + effect, sceneFile(), {});
        INFO(run.describe());
        CHECK(run.report->crtLooks->crtEffect == effect);
        const auto looks = looksOf(run);
        REQUIRE(looks.size() == presets.size());
        for (const auto& [id, preset] : presets) {
            INFO(id);
            CHECK(looks.at(id).preset == preset);
            CHECK(looks.at(id).rule == "video.json");
        }
    }
    // Without video.json's keys every screen is its kind's: consumer, the user's crt_presets.json over the shipped
    // values (here the consumer's scanlines off: its screen's rows lose their ripple), and a broken one left out.
    std::filesystem::remove(dir / "data" / "config" / "video.json");
    {
        const AppRun run = runScene(dir, "kinds", sceneFile(), {});
        INFO(run.describe());
        CHECK(run.report->crtLooks->presetsSource == "shipped");
        for (const auto& [id, look] : looksOf(run)) {
            INFO(id);
            CHECK(look.preset == "consumer");
            CHECK(look.rule == "kind");
        }
    }
    const auto scene = rv::scene::loadSceneDescription(sceneFile());
    REQUIRE(scene.has_value());
    const Ripple shipped = closeUp(dir, *scene, "crt_consumer", "kinds_close_consumer").ripple;
    REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "config" / "crt_presets.json",
                                          R"({"presets": {"consumer": {"scanlineStrength": 0}}})")
                .has_value());
    {
        // The user's value reaches the screen: the consumer tube's rows as flat as the neutral's, the same tube's
        // shipped scanlines rippling.
        const CloseUp user = closeUp(dir, *scene, "crt_consumer", "user_presets");
        INFO(user.run.describe());
        CHECK(user.run.report->crtLooks->presetsSource == "user");
        const auto looks = looksOf(user.run);
        REQUIRE(looks.contains("crt_consumer"));
        CHECK(looks.at("crt_consumer").preset == "consumer");
        CHECK(user.ripple.meanLuma > 40.0);
        CHECK(shipped.ripple > 0.02);
        CHECK(user.ripple.ripple < 0.005);
    }
    REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "config" / "crt_presets.json",
                                          R"({"presets": {"consumer": {"scanlineStrength": 4}}})")
                .has_value());
    CHECK(runScene(dir, "broken_presets", sceneFile(), {}).report->crtLooks->presetsSource == "shipped");
}
