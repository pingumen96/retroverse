// The benchmark path through the snes-stress room (I3 step 17b; docs/roadmap.md, I3, "Budgets"). Hidden ([.bench]:
// run by hand in Release, rv_smoke_tests "[benchmark]"): the snes-stress fixture (tests/fixtures/esde/snes-stress,
// 2,000 invented titles) recorded as the library of the case's data directory and imported with retroverse-cli; a first
// run without a lockstep makes the plan and fills the derived-texture cache, then retroverse.exe plays
// tests/input_scripts/bench_snes_room.json at 1920 x 1080 with --no-vsync (the frames never wait for the display) and
// --fixed-dt (the same path on every machine). The figures are printed (WARN) for docs/i3/step-17b.md: the frame times
// and the GPU time of every pass over the "benchmark" phase, what the last view drew, the GPU memory. In Release the
// budgets are checked: frame p99 at most 16.7 ms (NFR-03's 60 FPS) and p50 at most 8 ms; per pass, at its p99, shadow
// map 0.4 ms, depth pre-pass 0.4 ms, ambient occlusion 0.5 ms, scene 2.0 ms, post-processing 0.6 ms, outline 0.1 ms,
// their sum 5 ms.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/app/run_report.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

/// The benchmark path's frames: the phase holds frames 30 to 1259 of a 1,270-frame run (bench_snes_room.json).
constexpr std::uint64_t kBenchmarkFrames = 1230;

/// The budgets per pass (milliseconds; docs/roadmap.md, I3, "Budgets"), by GpuPassRecord name.
const std::vector<std::pair<std::string_view, double>> kPassBudgets{
    {"shadow", 0.4}, {"depth", 0.4}, {"ssao", 0.5}, {"scene", 2.0}, {"post", 0.6}, {"outline", 0.1}, {"total", 5.0}};

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
                                  {"--no-vsync", "--width", "1920", "--height", "1080", "--exit-after-frames",
                                   "--frames", "8000"});
    skipWithoutOpenGl(prepare);
    requireCleanRun(prepare);
    REQUIRE(prepare.report->world.state == "room");
    REQUIRE(prepare.report->residency.frontsResident == prepare.report->residency.items);
    REQUIRE(prepare.report->residency.spinesResident == prepare.report->residency.items);

    const AppRun run =
        runApp(retroverseExe(), room.dir, "benchmark",
               {"--width", "1920", "--height", "1080", "--no-vsync", "--exit-after-frames", "--frames", "1270",
                "--fixed-dt", "0.016667", "--input-script", arg(inputScript("bench_snes_room.json"))});
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
    // The budgets (Release, 1920 x 1080, --no-vsync, on the reference machine).
    CHECK(times.p99Ms <= 16.7);
    CHECK(times.p50Ms <= 8.0);
    for (const auto& [name, budget] : kPassBudgets) {
        if (const rv::app::GpuPassRecord* pass = passOf(benchmark->gpuPasses, name)) {
            INFO("GPU pass " << name << " p99 " << pass->times.p99Ms << " ms, budget " << budget << " ms");
            CHECK(pass->times.p99Ms <= budget);
        }
    }
#endif
}
