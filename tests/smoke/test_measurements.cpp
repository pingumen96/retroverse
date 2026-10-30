// The measurements of the budgets (I3 step 0d) in the run report of retroverse.exe: the frame times of the run with the
// warm-up left out, the phases an input script opens, the main thread's CPU sections, the renderer's GPU memory per
// kind; since the memory margin (docs/i5/memory-margin.md) the render targets' bytes at 1080p, with and without a
// screenshot.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <rv/app/frame_timing.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/surface_textures.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/render/texture_memory.hpp>
#include <rv/render/view_target_plan.hpp>

#include "smoke_support.hpp"

using namespace rv::smoke;

namespace {

/// Every map of every procedural surface the test room uses (all but the halls' wall wash, I6 step 2), RGBA8 with its
/// full chain (the metallic-roughness map R8 since I6 step 5's review) (each map uploaded once, in its own color space): the bytes and the count of the textures
/// gl::TextureCache generates for it.
struct SurfaceMaps {
    std::uint64_t bytes = 0;
    std::uint64_t maps = 0;
};

SurfaceMaps proceduralSurfaceMaps() {
    SurfaceMaps total;
    for (const rv::assets::ProceduralSurfaceInfo& surface : rv::assets::proceduralSurfaces()) {
        // The halls' wall wash, the lit shelves' glow (I6 step 3) and the packs' rugs (I6 step 3b) are not in the
        // test room.
        if (surface.name == "wallwash" || surface.name == "shelfglow" || surface.name == "ornate_rug_blue" ||
            surface.name == "ornate_rug_sage" || surface.name == "ornate_rug_green" ||
            surface.name == "geometric_rug" || surface.name == "runner_rug") {
            continue;
        }
        const std::uint64_t maps = surface.hasNormalMap ? 3 : 2;
        const int levels = rv::gl::fullMipLevelCount(surface.size);
        total.maps += maps;
        // The metallic-roughness map its roughness alone (R8: every surface is all metal or none).
        total.bytes += (maps - 1) * rv::gl::textureBytes(rv::gl::GpuFormat::Rgba8, surface.size, levels) +
                       rv::gl::textureBytes(rv::gl::GpuFormat::R8, surface.size, levels);
    }
    return total;
}

void checkOrdered(const rv::app::TimeSummary& times) {
    CHECK(times.p50Ms >= 0.0);
    CHECK(times.p50Ms <= times.p95Ms);
    CHECK(times.p95Ms <= times.p99Ms);
    CHECK(times.p99Ms <= times.maxMs);
}

/// The render targets' bytes of a run at their highest.
std::uint64_t peakTargetBytes(const AppRun& run) {
    return run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::Targets).peakBytes;
}

const rv::app::CpuSectionRecord* findSection(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::CpuSectionRecord& section : report.cpuSections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("measurements/the test room reports its frame times, scripted phases, CPU sections and GPU memory",
          "[gpu][measurements]") {
    const std::filesystem::path caseDir = freshCaseDir("measurements");
    constexpr std::uint64_t kFrames = 100;
    const AppRun run =
        runApp(retroverseExe(), caseDir, "phases",
               {"--scene", "m0_testroom", "--spawn", "0,0,3.2,0,-6", "--exit-after-frames", "--frames",
                std::to_string(kFrames), "--fixed-dt", "0.016667", "--input-script",
                arg(sourceDir() / "tests" / "input_scripts" / "measurement_phases.json")});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;

    // The run: its 100 frames but the first 30.
    CHECK(report.frameTimes.warmUpFrames == rv::app::kFrameTimeWarmUpFrames);
    REQUIRE(report.frameTimes.times.has_value());
    const rv::app::TimeSummary& run100 = *report.frameTimes.times;
    CHECK(run100.count == kFrames - rv::app::kFrameTimeWarmUpFrames);
    checkOrdered(run100);
    CHECK(run100.p50Ms > 0.0);

    // The script's phases, in the order they opened (measurement_phases.json).
    REQUIRE(report.phases.size() == 2);
    const rv::app::PhaseRecord& coverLoad = report.phases[0];
    CHECK(coverLoad.name == "coverLoad");
    CHECK(coverLoad.opened == 1);
    CHECK_FALSE(coverLoad.open);
    CHECK(coverLoad.frames == 40); // frames 10 to 49
    REQUIRE(coverLoad.times.has_value());
    CHECK(coverLoad.times->count == 20); // 30 to 49: the others are the run's warm-up
    checkOrdered(*coverLoad.times);
    const rv::app::PhaseRecord& benchmark = report.phases[1];
    CHECK(benchmark.name == "benchmark");
    CHECK(benchmark.opened == 1);
    CHECK_FALSE(benchmark.open);
    CHECK(benchmark.frames == 40); // frames 40 to 79
    REQUIRE(benchmark.times.has_value());
    CHECK(benchmark.times->count == 40);
    checkOrdered(*benchmark.times);
    CHECK(benchmark.times->maxMs <= run100.maxMs);

    // The CPU sections of every frame: the product UI's update and the scene renderer's views.
    for (const std::string_view name : {rv::core::cpu_section::kProductUi, rv::core::cpu_section::kSceneSubmit}) {
        CAPTURE(name);
        const rv::app::CpuSectionRecord* section = findSection(report, name);
        REQUIRE(section != nullptr);
        CHECK(section->times.count == kFrames);
        checkOrdered(section->times);
    }
    // Those of later steps did not run.
    CHECK(findSection(report, rv::core::cpu_section::kCollectionPlanning) == nullptr);
    CHECK(findSection(report, rv::core::cpu_section::kLayerUploads) == nullptr);
    CHECK(findSection(report, rv::core::cpu_section::kVideoUpload) == nullptr);

    // The GPU memory: the test room uses every procedural surface but the wall wash, whose maps take exactly what
    // their formats and sizes give (about 80 MiB); nothing of the collection arrays, the hero textures or the video exists yet.
    const rv::app::GpuMemoryRecord& memory = report.gpuMemory;
    const SurfaceMaps surfaces = proceduralSurfaceMaps();
    CHECK(memory.usage(rv::gl::GpuMemoryKind::ProceduralSurfaces) ==
          rv::gl::GpuMemoryUsage{surfaces.bytes, surfaces.bytes, surfaces.maps});
    for (const rv::gl::GpuMemoryKind kind :
         {rv::gl::GpuMemoryKind::CollectionArrays, rv::gl::GpuMemoryKind::HeroTextures, rv::gl::GpuMemoryKind::Video}) {
        CAPTURE(rv::gl::toString(kind));
        CHECK(memory.usage(kind) == rv::gl::GpuMemoryUsage{});
    }
    // The view targets of the window and the shadow map; the scene's own textures (labels, posters, the TV's picture)
    // and the four defaults (the run would have failed unless the accounting recorded exactly what the texture cache
    // and the defaults hold: SceneGraphics::finishRun); meshes, instances and uniform blocks.
    CHECK(memory.usage(rv::gl::GpuMemoryKind::Targets).bytes > 0);
    CHECK(memory.usage(rv::gl::GpuMemoryKind::SceneTextures).allocations > 4);
    CHECK(memory.usage(rv::gl::GpuMemoryKind::Buffers).bytes > 0);
    // Every texture of the room within the 256 MiB budget of ADR-0011 §9.
    const std::uint64_t textures = memory.totalBytes - memory.usage(rv::gl::GpuMemoryKind::Buffers).bytes;
    CHECK(textures < std::uint64_t{256} * 1024 * 1024);
    INFO("textures " << textures << " bytes, buffers " << memory.usage(rv::gl::GpuMemoryKind::Buffers).bytes);

    // A run without phases holds none; with no more frames than the warm-up, no frame time.
    const AppRun shortRun = runApp(retroverseExe(), caseDir, "no_phases",
                                   {"--scene", "m0_testroom", "--exit-after-frames", "--frames", "30"});
    requireCleanRun(shortRun);
    INFO(shortRun.describe());
    CHECK(shortRun.report->phases.empty());
    CHECK(shortRun.report->frameTimes == rv::app::FrameTimesRecord{rv::app::kFrameTimeWarmUpFrames, std::nullopt});
    CHECK(shortRun.report->gpuMemory.usage(rv::gl::GpuMemoryKind::ProceduralSurfaces) ==
          memory.usage(rv::gl::GpuMemoryKind::ProceduralSurfaces));
}

TEST_CASE("measurements/the render targets at 1080p: one set at a time, a screenshot at the window's size through "
          "the window's",
          "[gpu][measurements][targets]") {
    const std::filesystem::path caseDir = freshCaseDir("measurements_targets");
    // The memory margin's figure: the view targets of a 1920 x 1080 output (44.2 MiB) and the key light's shadow map
    // (16 MiB) under 64 MiB (88.5 MiB before it, 161.0 with a screenshot of the window's size).
    constexpr std::uint64_t kTargetsAt1080 = std::uint64_t{64} * 1024 * 1024;
    const std::uint64_t shadowMap = rv::gl::textureBytes(rv::gl::GpuFormat::Depth32F,
                                                         glm::ivec2(rv::gl::kShadowMapResolution), 1);
    const std::uint64_t set1080 = rv::gl::viewTargetsBytes(glm::ivec2(1920, 1080));
    CHECK(set1080 + shadowMap < kTargetsAt1080);

    const std::vector<std::string> scene{"--scene",    "m0_testroom", "--spawn",
                                         "0,0,3.2,0,-6", "--fixed-dt", "0.016667",
                                         "--exit-after-frames", "--frames", "3"};
    auto withScene = [&scene](std::vector<std::string> more) {
        more.insert(more.begin(), scene.begin(), scene.end());
        return more;
    };
    const auto drawableOf = [](const AppRun& run) {
        REQUIRE(run.report->window.has_value());
        return glm::ivec2(run.report->window->drawableWidth, run.report->window->drawableHeight);
    };

    // A window asked for at 1920 x 1080, without a screenshot: its drawable (that size times the display's scale, with
    // highDpi) is what the window's targets are allocated for. The screenshots below take the drawable's size, so that
    // one of them has the window's size whatever the scale.
    const AppRun window = runApp(retroverseExe(), caseDir, "window_1080",
                                 withScene({"--width", "1920", "--height", "1080"}));
    skipWithoutOpenGl(window);
    requireCleanRun(window);
    INFO(window.describe());
    const glm::ivec2 drawable = drawableOf(window);
    INFO("the window's drawable: " << drawable.x << " x " << drawable.y);
    REQUIRE(drawable.x > 64);
    REQUIRE(drawable.y > 64);
    const std::uint64_t windowSet = rv::gl::viewTargetsBytes(drawable);
    CHECK(peakTargetBytes(window) == windowSet + shadowMap);
    if (drawable == glm::ivec2(1920, 1080)) {
        CHECK(peakTargetBytes(window) <= kTargetsAt1080);
    }
    const std::string shotSize = std::to_string(drawable.x) + "x" + std::to_string(drawable.y);

    // A screenshot of that size from a window too small to matter: drawn through targets of its own, with the window's
    // let go of while they exist, so the run's highest is the screenshot's set and the shadow map, exactly.
    const AppRun small = runApp(retroverseExe(), caseDir, "small_window",
                                withScene({"--width", "64", "--height", "64", "--screenshot",
                                           arg(caseDir / "small_window.png"), "--screenshot-size", shotSize}));
    requireCleanRun(small);
    INFO(small.describe());
    REQUIRE(small.report->screenshot.has_value());
    REQUIRE(small.report->screenshot->targets == rv::app::kScreenshotOwnTargets);
    const glm::ivec2 smallDrawable = drawableOf(small);
    REQUIRE(smallDrawable != drawable);
    CHECK(peakTargetBytes(small) == windowSet + shadowMap);
    // The window's set made again after it: the run ends holding the shadow map and the small window's targets.
    CHECK(small.report->gpuMemory.usage(rv::gl::GpuMemoryKind::Targets).bytes ==
          rv::gl::viewTargetsBytes(smallDrawable) + shadowMap);

    // The same window with a screenshot of its size: drawn through the window's targets, it takes none of its own.
    const AppRun shot = runApp(retroverseExe(), caseDir, "window_1080_screenshot",
                               withScene({"--width", "1920", "--height", "1080", "--screenshot",
                                          arg(caseDir / "window_1080.png"), "--screenshot-size", shotSize}));
    requireCleanRun(shot);
    INFO(shot.describe());
    REQUIRE(drawableOf(shot) == drawable);
    REQUIRE(shot.report->screenshot.has_value());
    REQUIRE(shot.report->screenshot->targets == rv::app::kScreenshotWindowTargets);
    INFO("targets at their highest: window " << peakTargetBytes(window) << " B, with the screenshot "
                                             << peakTargetBytes(shot) << " B");
    CHECK(peakTargetBytes(shot) == peakTargetBytes(window));

    // The screenshot drawn through the window's targets is the one drawn through targets of its own.
    const auto own = rv::assets::loadImage(caseDir / "small_window.png", 3);
    const auto shared = rv::assets::loadImage(caseDir / "window_1080.png", 3);
    REQUIRE(own.has_value());
    REQUIRE(shared.has_value());
    CHECK(own->width == drawable.x);
    CHECK(own->height == drawable.y);
    CHECK(own->width == shared->width);
    CHECK(own->height == shared->height);
    CHECK(own->pixels == shared->pixels);
}
