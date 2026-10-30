// The benchmark path through the snes-stress room (I3 step 17b). Hidden ([.bench]:
// run by hand in Release, rv_smoke_tests "[benchmark]"): the snes-stress fixture (tests/fixtures/esde/snes-stress,
// 2,000 invented titles) recorded as the library of the case's data directory and imported with retroverse-cli; a first
// run without a lockstep makes the plan and fills the derived-texture cache, then retroverse.exe plays
// tests/input_scripts/bench_snes_room.json at 1920 x 1080 with --no-vsync (the frames never wait for the display) and
// --fixed-dt (the same path on every machine). The figures are printed (WARN) for docs/i3/step-17b.md: the frame times
// and the GPU time of every pass over the "benchmark" phase, what the last view drew, the GPU memory. In Release the
// frame times must be acceptable (docs/roadmap.md, "Acceptable performance"): p99 at most 33.3 ms (never under 30 FPS)
// and p50 at most 16.7 ms (60 FPS typically); the GPU passes are only reported.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <glm/vec2.hpp>

#include <rv/app/run_report.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

/// The benchmark path's frames: the phase holds frames 30 to 1259 of a 1,270-frame run (bench_snes_room.json).
constexpr std::uint64_t kBenchmarkFrames = 1230;

/// The snes-stress fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-stress").value()),
          dir(freshCaseDir(std::string("world_benchmark/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseExe().parent_path() / "retroverse-cli.exe", dir,
                                      std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }
};

const rv::app::PhaseRecord* phaseOf(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::PhaseRecord& phase : report.phases) {
        if (phase.name == name) {
            return &phase;
        }
    }
    return nullptr;
}

const rv::app::GpuPassRecord* passOf(const std::vector<rv::app::GpuPassRecord>& passes, std::string_view name) {
    for (const rv::app::GpuPassRecord& pass : passes) {
        if (pass.name == name) {
            return &pass;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("world benchmark: the benchmark path through the snes-stress room (2,000 titles)",
          "[.bench][gpu][world][benchmark]") {
    ImportedRoom room("stress");
    // The plan made and every layer in the cache, as a restart finds them: a run without a lockstep (which would wait
    // only for the layers in view), long enough for the cold cache (8 to 10 s for 2,000 titles, I3 step 12).
    const AppRun prepare = runApp(retroverseExe(), room.dir, "prepare",
                                  {"--start-room", "room:snes", "--no-vsync", "--width", "1920", "--height",
                                   "1080", "--exit-after-frames", "--frames", "8000"});
    skipWithoutOpenGl(prepare);
    requireCleanRun(prepare);
    REQUIRE(prepare.report->world.state == "room");
    REQUIRE(prepare.report->residency.frontsResident == prepare.report->residency.items);
    REQUIRE(prepare.report->residency.spinesResident == prepare.report->residency.items);

    const AppRun run =
        runApp(retroverseExe(), room.dir, "benchmark",
               {"--start-room", "room:snes", "--width", "1920", "--height", "1080", "--no-vsync",
                "--exit-after-frames", "--frames", "1270", "--fixed-dt", "0.016667", "--input-script",
                arg(inputScript("bench_snes_room.json"))});
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.world.state == "room");
    CHECK(report.world.items == 2000);
    CHECK(report.world.planDigest == prepare.report->world.planDigest);
    CHECK(report.residency.frontsResident == report.residency.items);
    CHECK(report.residency.spinesResident == report.residency.items);

    const rv::app::PhaseRecord* benchmark = phaseOf(report, "benchmark");
    REQUIRE(benchmark != nullptr);
    CHECK(benchmark->opened == 1);
    CHECK_FALSE(benchmark->open);
    CHECK(benchmark->frames == kBenchmarkFrames);
    REQUIRE(benchmark->times.has_value());
    CHECK(benchmark->times->count == kBenchmarkFrames);
    const rv::app::TimeSummary& times = *benchmark->times;

    std::string passes;
    for (const rv::app::GpuPassRecord& pass : benchmark->gpuPasses) {
        passes += fmt::format("{}{} p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f} ms ({})", passes.empty() ? "" : "; ",
                              pass.name, pass.times.p50Ms, pass.times.p95Ms, pass.times.p99Ms, pass.times.maxMs,
                              pass.times.count);
    }
    std::string view;
    if (report.lastView) {
        view = fmt::format("{} draw calls ({} static), {} instances, {} triangles, {} clusters ({} culled), shadow {} "
                           "draw calls, depth {} draw calls",
                           report.lastView->drawCalls, report.lastView->staticDrawCalls, report.lastView->instances,
                           report.lastView->triangles, report.lastView->clusters, report.lastView->culledClusters,
                           report.lastView->shadowDrawCalls, report.lastView->depthDrawCalls);
    }
    WARN(fmt::format("snes-stress benchmark ({}, {}): phase {} frames, frame p50 {:.2f} ms, p95 {:.2f} ms, p99 {:.2f} "
                     "ms, max {:.2f} ms; GPU {}; the last view: {}; arrays {:.1f} MiB, GPU memory peak {:.1f} MiB; "
                     "the walker ended at {}; every layer resident {:.0f} ms after the room showed (cache {} hits, {} "
                     "misses)",
                     report.buildType, report.context ? report.context->renderer : std::string("?"), times.count,
                     times.p50Ms, times.p95Ms, times.p99Ms, times.maxMs, passes, view,
                     static_cast<double>(report.residency.arrayBytes) / (1024.0 * 1024.0),
                     static_cast<double>(report.gpuMemory.peakTotalBytes) / (1024.0 * 1024.0),
                     report.finalCamera ? spawnArgument(*report.finalCamera) : std::string("?"),
                     report.residency.residentAfterShownMs.value_or(-1.0), report.residency.cacheHits,
                     report.residency.cacheMisses));

    for (const std::string_view name : {"shadow", "depth", "ssao", "scene", "post", "total"}) {
        INFO("GPU pass " << name);
        CHECK(passOf(benchmark->gpuPasses, name) != nullptr);
    }
#if defined(NDEBUG)
    // Acceptable frame times (Release, 1920 x 1080, --no-vsync).
    CHECK(times.p99Ms <= 33.3);
    CHECK(times.p50Ms <= 16.7);
#endif
}

TEST_CASE("world benchmark: the benchmark path in the fake headset at Link's size (I10 step 7)",
          "[.bench][gpu][world][benchmark][xr]") {
    // ADR-0017 §9: both eyes at the runtime's recommended size (Link: 2080 x 2096 on a Quest 3S; the fake runtime's
    // 1024 x 1024 at render scale 2, 2048 x 2048), render scale 1 (frame-limited runs keep it: no dynamic scale), the
    // fake runtime pacing nothing, the same path as the desktop's; the head stands still, the walker walks and turns.
    ImportedRoom room("vr_stress");
    const AppRun prepare = runApp(retroverseExe(), room.dir, "prepare",
                                  {"--start-room", "room:snes", "--no-vsync", "--width", "1920", "--height",
                                   "1080", "--exit-after-frames", "--frames", "8000"});
    skipWithoutOpenGl(prepare);
    requireCleanRun(prepare);
    REQUIRE(prepare.report->world.state == "room");

    const AppRun run =
        runApp(retroverseExe(), room.dir, "benchmark",
               {"--vr", "--xr-runtime", "fake", "--xr-render-scale", "2", "--start-room", "room:snes", "--no-vsync",
                "--exit-after-frames", "--frames", "1270", "--fixed-dt", "0.016667", "--input-script",
                arg(inputScript("bench_snes_room.json"))});
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.world.state == "room");
    REQUIRE(report.xr);
    CHECK(report.xr->eyeSize == glm::ivec2(2048, 2048));
    CHECK(report.xr->framesWithViews == 1270);
    const rv::app::PhaseRecord* benchmark = phaseOf(report, "benchmark");
    REQUIRE(benchmark != nullptr);
    REQUIRE(benchmark->times.has_value());
    const rv::app::TimeSummary& times = *benchmark->times;
    // The GPU passes are the last view's (the right eye's): both eyes take about twice the total.
    const rv::app::GpuPassRecord* total = passOf(benchmark->gpuPasses, "total");
    REQUIRE(total != nullptr);
    std::string passes;
    for (const rv::app::GpuPassRecord& pass : benchmark->gpuPasses) {
        passes += fmt::format("{}{} p50 {:.3f} p99 {:.3f} ms", passes.empty() ? "" : "; ", pass.name, pass.times.p50Ms,
                              pass.times.p99Ms);
    }
    WARN(fmt::format("VR benchmark ({}, {}): phase {} frames, frame p50 {:.2f} ms, p95 {:.2f} ms, p99 {:.2f} ms, max "
                     "{:.2f} ms; both eyes' GPU about p50 {:.2f} ms, p99 {:.2f} ms; one eye: {}; the session's CPU p99 "
                     "{:.2f} ms",
                     report.buildType, report.context ? report.context->renderer : std::string("?"), times.count,
                     times.p50Ms, times.p95Ms, times.p99Ms, times.maxMs, 2.0 * total->times.p50Ms,
                     2.0 * total->times.p99Ms, passes, report.xr->cpuTimes ? report.xr->cpuTimes->p99Ms : -1.0));
#if defined(NDEBUG)
    // ADR-0017 §9 at 72 Hz: a frame within the display's period typically, never far beyond it.
    CHECK(times.p50Ms <= 13.9);
    CHECK(times.p99Ms <= 20.0);
#endif
}
