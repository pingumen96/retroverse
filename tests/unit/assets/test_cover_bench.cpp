// I3 step 5: timings of the cover and label pipeline (ADR-0011 §9), hidden ([.bench]) and meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[bench][covers]"
// The budget: one 680 x 497 PNG cover to its BC1 layer (decode, fit, mips in linear light, BC1) within 25 ms median on
// one worker. Informative: each stage alone, the spine and the generated label, the cache's cold and warm paths, and
// the throughput of the worker pool. The covers are generated (gradients, shapes, fine bars and noise, so that their
// PNG files compress about as poorly as the survey's scanned covers). Timings taken while other work runs are
// informative; the gate measures on a quiet machine (docs/i3/step-5.md records the figures).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rv/assets/bc1.hpp>
#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/worker_pool.hpp>

#include "cover_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::assets;
using Clock = std::chrono::steady_clock;

namespace {

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

rv::core::DurationPercentiles summarize(std::vector<double> samples) {
    return rv::core::summarizeDurations(samples).value_or(rv::core::DurationPercentiles{});
}

void report(const char* what, const rv::core::DurationPercentiles& times) {
    std::cout << fmt::format("{:<44} p50 {:7.3f} ms  p95 {:7.3f} ms  max {:7.3f} ms  ({} samples)\n", what,
                             times.p50Ms, times.p95Ms, times.maxMs, times.count);
}

/// Photo-like covers: the survey's are 680 x 497 PNGs of about 530 KiB.
std::vector<std::vector<std::byte>> coverFiles(int count, int width, int height) {
    std::vector<std::vector<std::byte>> files;
    for (int i = 0; i < count; ++i) {
        files.push_back(encodePng(rv::test::covers::makeCover(width, height, 1000 + static_cast<std::uint64_t>(i)))
                            .value());
    }
    return files;
}

} // namespace

TEST_CASE("assets/covers bench: one 680 x 497 PNG cover to BC1 within 25 ms median", "[.bench][bench][covers]") {
    const std::vector<std::vector<std::byte>> files = coverFiles(40, 680, 497);
    std::cout << fmt::format("Generated cover PNG: {} KiB\n", files.front().size() / 1024);
    std::vector<double> whole;
    std::vector<double> decode;
    std::vector<double> fit;
    std::vector<double> mips;
    std::vector<double> bc1;
    for (const std::vector<std::byte>& file : files) {
        auto start = Clock::now();
        auto layer = buildCoverLayer(file, TextureFormat::Bc1Srgb, "bench");
        whole.push_back(millisecondsSince(start));
        REQUIRE(layer.has_value());

        start = Clock::now();
        auto image = decodeImage(file, 4, "bench");
        decode.push_back(millisecondsSince(start));
        REQUIRE(image.has_value());
        start = Clock::now();
        auto fitted = fitCover(*image, 256, 188);
        fit.push_back(millisecondsSince(start));
        REQUIRE(fitted.has_value());
        start = Clock::now();
        const std::vector<ImageData> chain = buildMipChain(fitted->picture);
        mips.push_back(millisecondsSince(start));
        start = Clock::now();
        for (const ImageData& level : chain) {
            REQUIRE_FALSE(encodeBc1(level).empty());
        }
        bc1.push_back(millisecondsSince(start));
    }
    const auto total = summarize(whole);
    report("cover to BC1 layer (decode, fit, mips, BC1)", total);
    report("  decode (stb_image, PNG)", summarize(decode));
    report("  fit (stb_image_resize2, sRGB)", summarize(fit));
    report("  mip chain (linear light)", summarize(mips));
    report("  BC1 (stb_dxt, high quality, 9 levels)", summarize(bc1));
    CHECK(total.p50Ms <= 25.0);
}

TEST_CASE("assets/covers bench: spines, generated labels and the cache", "[.bench][bench][covers]") {
    const auto& bundled = rv::test::covers::bundledFonts();
    const LabelFonts fonts{&bundled.title, &bundled.detail};
    std::vector<double> spines;
    std::vector<double> labels;
    for (int i = 0; i < 60; ++i) {
        const std::string title = i % 2 == 0 ? "Starlight Brigade - The Clockwork Armada " + std::to_string(i)
                                             : "Comet Courier " + std::to_string(i);
        auto start = Clock::now();
        auto spine = buildSpineLayer(fonts, SpineRequest{title, "", std::nullopt, static_cast<std::uint64_t>(i)},
                                     TextureFormat::Bc1Srgb);
        spines.push_back(millisecondsSince(start));
        REQUIRE(spine.has_value());
        start = Clock::now();
        auto label = buildFrontLabelLayer(
            fonts, FrontLabelRequest{title, "", "Fantasy Home System", "1994", static_cast<std::uint64_t>(i)},
            TextureFormat::Bc1Srgb);
        labels.push_back(millisecondsSince(start));
        REQUIRE(label.has_value());
    }
    report("spine (layout, text at 2x, mips, BC1)", summarize(spines));
    report("generated front label", summarize(labels));

    // The cache: a cold serve (read, build, store), a warm one (read and check the entry).
    const rv::test::TempDir temp("rv-cover-bench");
    const std::vector<std::vector<std::byte>> files = coverFiles(30, 680, 497);
    std::vector<std::filesystem::path> sources;
    for (std::size_t i = 0; i < files.size(); ++i) {
        sources.push_back(temp.path() / "media" / "snes" / "covers" / fmt::format("Game {}.png", i));
        REQUIRE(rv::core::writeFileAtomic(sources.back(), files[i]).has_value());
    }
    const std::filesystem::path root = temp.path() / "cache";
    std::vector<double> cold;
    std::vector<double> warm;
    {
        DerivedTextureCache cache(root);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto start = Clock::now();
            auto served = serveCover(cache, TextureFormat::Bc1Srgb, "covers", fmt::format("snes/covers/Game {}.png", i),
                                     sources[i]);
            cold.push_back(millisecondsSince(start));
            REQUIRE(served.has_value());
        }
    }
    {
        DerivedTextureCache cache(root);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto start = Clock::now();
            auto served = serveCover(cache, TextureFormat::Bc1Srgb, "covers", fmt::format("snes/covers/Game {}.png", i),
                                     sources[i]);
            warm.push_back(millisecondsSince(start));
            REQUIRE(served.has_value());
            REQUIRE(served->outcome == ServeOutcome::CacheHit);
        }
    }
    report("cache, cold (read, build, store)", summarize(cold));
    report("cache, warm (source checked, entry read)", summarize(warm));

    // The worker pool's throughput on the cold path (ADR-0011 §6: min(6, hardware threads - 3) threads).
    const std::filesystem::path poolRoot = temp.path() / "pool-cache";
    DerivedTextureCache cache(poolRoot);
    rv::core::WorkerPool pool;
    std::atomic<int> done{0};
    const auto start = Clock::now();
    for (int round = 0; round < 4; ++round) {
        for (std::size_t i = 0; i < sources.size(); ++i) {
            REQUIRE(pool.submit([&, i, round](const rv::core::JobContext&) {
                            auto served = serveCover(cache, TextureFormat::Bc1Srgb, "covers",
                                                     fmt::format("snes/covers/Game {} {}.png", i, round), sources[i]);
                            if (served) {
                                ++done;
                            }
                        }).has_value());
        }
    }
    REQUIRE(pool.waitIdle(std::chrono::seconds(120)));
    const double elapsed = millisecondsSince(start);
    std::cout << fmt::format("worker pool: {} threads, {} covers cold in {:.0f} ms ({:.1f} covers/s)\n",
                             pool.threadCount(), done.load(), elapsed, done.load() * 1000.0 / elapsed);
    CHECK(done == 4 * static_cast<int>(sources.size()));
}
