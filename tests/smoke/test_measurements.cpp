// The measurements of the budgets (I3 step 0d) in the run report of retroverse.exe: the frame times of the run with the
// warm-up left out, the phases an input script opens, the main thread's CPU sections, the renderer's GPU memory per
// kind.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <rv/app/frame_timing.hpp>
#include <rv/assets/surface_textures.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_memory.hpp>

#include "smoke_support.hpp"

using namespace rv::smoke;

namespace {

/// Every map of every procedural surface the test room uses (all but the halls' wall wash, I6 step 2), RGBA8 with its
/// full chain (each map uploaded once, in its own color space): the bytes and the count of the textures
/// gl::TextureCache generates for it.
struct SurfaceMaps {
    std::uint64_t bytes = 0;
    std::uint64_t maps = 0;
};

SurfaceMaps proceduralSurfaceMaps() {
    SurfaceMaps total;
    for (const rv::assets::ProceduralSurfaceInfo& surface : rv::assets::proceduralSurfaces()) {
        // The halls' wall wash and the lit shelves' glow (I6 step 3) are not in the test room.
        if (surface.name == "wallwash" || surface.name == "shelfglow") {
            continue;
        }
        const std::uint64_t maps = surface.hasNormalMap ? 3 : 2;
        total.maps += maps;
        total.bytes += maps * rv::gl::textureBytes(rv::gl::GpuFormat::Rgba8, surface.size,
                                                   rv::gl::fullMipLevelCount(surface.size));
    }
    return total;
}

void checkOrdered(const rv::app::TimeSummary& times) {
    CHECK(times.p50Ms >= 0.0);
    CHECK(times.p50Ms <= times.p95Ms);
    CHECK(times.p95Ms <= times.p99Ms);
    CHECK(times.p99Ms <= times.maxMs);
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
