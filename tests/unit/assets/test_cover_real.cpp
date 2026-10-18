// I3 step 5: the cover pipeline measured on a sample of the real library's SNES covers (ADR-0011 §9). Hidden
// ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed and read-only snapshots of
// the library before and after, the media root in RV_REAL_MEDIA_ROOT:
//   ./scripts/real-library.ps1 -Filter "[real-library][covers]"   (and the other folders, as the script asks)
// The covers are only opened for reading (core::SharedFileReader, full sharing); the derived-texture cache goes under
// build/<preset>/real-library-covers (emptied first, so the first pass is cold), never into the library. Nothing of
// the user's (names, pixels) is printed: only counts and timings.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/assets/bc1.hpp>
#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "cover_test_support.hpp"

namespace fs = std::filesystem;
using namespace rv::assets;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kSample = 120;

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void report(const char* what, std::vector<double> samples) {
    const auto times = rv::core::summarizeDurations(samples).value_or(rv::core::DurationPercentiles{});
    std::cout << fmt::format("{:<46} p50 {:7.2f} ms  p95 {:7.2f} ms  max {:7.2f} ms  ({} covers)\n", what, times.p50Ms,
                             times.p95Ms, times.maxMs, times.count);
}

} // namespace

TEST_CASE("assets/covers: a sample of the real SNES covers through the pipeline and the cache",
          "[assets][covers][.real-library]") {
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!mediaRoot) {
        SKIP("RV_REAL_MEDIA_ROOT is not set: run through scripts/real-library.ps1");
    }
    const fs::path covers = *mediaRoot / "snes" / "covers";
    REQUIRE(fs::is_directory(covers));

    // The cache under build/, never in the library (the data-directory guard of ADR-0008 §6 and §7).
    const fs::path cacheRoot = rv::core::executablePath().parent_path().parent_path() / "real-library-covers";
    const fs::path protectedRoots[] = {*mediaRoot};
    REQUIRE(rv::core::checkUserDataDirectory(cacheRoot, protectedRoots).has_value());
    std::error_code ignored;
    fs::remove_all(cacheRoot, ignored);

    // Every n-th cover of the folder's sorted listing, sub-folders included (read-only), sizes and times kept to check
    // afterwards.
    std::vector<fs::path> all;
    for (const auto& entry : fs::recursive_directory_iterator(covers)) {
        if (entry.is_regular_file()) {
            all.push_back(entry.path());
        }
    }
    std::sort(all.begin(), all.end());
    REQUIRE(all.size() >= kSample);
    std::vector<fs::path> sample;
    for (std::size_t i = 0; i < kSample; ++i) {
        sample.push_back(all[i * all.size() / kSample]);
    }
    std::map<fs::path, std::pair<std::uintmax_t, fs::file_time_type>> before;
    std::uintmax_t sampleBytes = 0;
    for (const fs::path& path : sample) {
        before[path] = {fs::file_size(path), fs::last_write_time(path)};
        sampleBytes += fs::file_size(path);
    }
    std::cout << fmt::format("{} covers in the folder, a sample of {} ({:.1f} MiB)\n", all.size(), sample.size(),
                             static_cast<double>(sampleBytes) / (1024.0 * 1024.0));

    const auto relative = [&](const fs::path& path) {
        return rv::core::pathToGenericUtf8(path.lexically_relative(*mediaRoot));
    };

    // Cold: read, decode, fit, mips, BC1, store.
    std::vector<double> cold;
    std::map<std::string, int> modes;
    std::map<std::string, int> sizes;
    std::vector<Rgb8> dominants;
    {
        DerivedTextureCache cache(cacheRoot);
        for (const fs::path& path : sample) {
            const auto start = Clock::now();
            auto served = serveCover(cache, TextureFormat::Bc1Srgb, "covers", relative(path), path);
            cold.push_back(millisecondsSince(start));
            REQUIRE(served.has_value());
            CHECK(served->outcome == ServeOutcome::Built);
            dominants.push_back(served->texture.dominant);
        }
        CHECK(cache.stats().stored == sample.size());
    }
    report("cold: read, decode, fit, mips, BC1, store", cold);

    // Warm: a new run finds every entry, the sources checked by size and time.
    std::vector<double> warm;
    {
        DerivedTextureCache cache(cacheRoot);
        for (const fs::path& path : sample) {
            const auto start = Clock::now();
            auto served = serveCover(cache, TextureFormat::Bc1Srgb, "covers", relative(path), path);
            warm.push_back(millisecondsSince(start));
            REQUIRE(served.has_value());
            CHECK(served->outcome == ServeOutcome::CacheHit);
        }
        CHECK(cache.stats().hits == sample.size());
    }
    report("warm: the source checked, the entry read", warm);

    // The fit decisions, BC1 against RGBA8, and the spines on the covers' colours after encoding.
    // From memory (the CPU's part of the cold path, the budget's "one cover on one worker"), the fit decisions, BC1
    // against RGBA8 at level 0 (256 x 188, seen within about 1 m) and level 2 (64 x 47, about what a shelf at 3 m
    // samples), and the spines on the covers' colours after encoding.
    std::vector<double> build;
    std::vector<double> psnr;
    std::vector<double> psnrLevel2;
    double worstContrast = 100.0;
    const auto& bundled = rv::test::covers::bundledFonts();
    const LabelFonts fonts{&bundled.title, &bundled.detail};
    for (std::size_t i = 0; i < sample.size(); ++i) {
        auto file = rv::core::readFileShared(sample[i], kMaxImageFileBytes);
        REQUIRE(file.has_value());
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(file->data()), file->size());
        const auto start = Clock::now();
        auto layer = buildCoverLayer(bytes, TextureFormat::Bc1Srgb, "cover");
        build.push_back(millisecondsSince(start));
        REQUIRE(layer.has_value());

        auto image = decodeImage(bytes, 4, "cover");
        REQUIRE(image.has_value());
        ++sizes[fmt::format("{}x{}", image->width, image->height)];
        auto fitted = fitCover(*image, 256, 188);
        REQUIRE(fitted.has_value());
        ++modes[std::string(toString(fitted->mode))];
        CHECK(fitted->cropShare <= kMaxCoverCropShare);
        const std::vector<ImageData> chain = buildMipChain(fitted->picture);
        psnr.push_back(psnrRgb(chain[0], decodeLevel(layer->texture, 0)));
        psnrLevel2.push_back(psnrRgb(chain[2], decodeLevel(layer->texture, 2)));

        // The file name stands in for the title (the spine is derived data: it stays in memory).
        auto spine = buildSpineLayer(
            fonts, SpineRequest{rv::core::pathToUtf8(sample[i].stem()), "", dominants[i], 0}, TextureFormat::Bc1Srgb);
        REQUIRE(spine.has_value());
        const ImageData decoded = decodeLevel(spine->texture, 0);
        double contrast = 1.0;
        const Rgb8 background = spine->colours.background;
        for (std::size_t p = 0; p < decoded.pixels.size(); p += 4) {
            contrast = std::max(contrast, contrastRatio(background, {decoded.pixels[p], decoded.pixels[p + 1],
                                                                     decoded.pixels[p + 2]}));
        }
        worstContrast = std::min(worstContrast, contrast);
    }
    report("from memory: decode, fit, mips, BC1", build);
    for (std::vector<double>* values : {&psnr, &psnrLevel2}) {
        std::sort(values->begin(), values->end());
        std::cout << fmt::format("BC1 against RGBA8, level {}: worst {:.2f} dB, median {:.2f} dB, best {:.2f} dB\n",
                                 values == &psnr ? 0 : 2, values->front(), (*values)[values->size() / 2],
                                 values->back());
    }
    std::cout << fmt::format("spine text contrast after BC1, worst: {:.2f}:1\n", worstContrast);
    for (const auto& [mode, count] : modes) {
        std::cout << fmt::format("fit {}: {}\n", mode, count);
    }
    for (const auto& [size, count] : sizes) {
        std::cout << fmt::format("source size {}: {}\n", size, count);
    }
    // The roadmap's 32 dB is checked on the fixtures' covers (test_bc1.cpp); scanned covers hold more fine detail
    // (small print, screenshots) and measure lower at 256 px: here a floor that only a broken encoder would miss.
    CHECK(psnr.front() >= 24.0);
    CHECK(worstContrast >= kMinTextContrast);

    // Read-only: every sampled cover as it was.
    for (const fs::path& path : sample) {
        CHECK(fs::file_size(path) == before[path].first);
        CHECK(fs::last_write_time(path) == before[path].second);
    }
}
