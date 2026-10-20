// The collection's texture arrays (I3 step 6, ADR-0011 §4 and §6) through rv_collection_probe (tests/collection_probe):
// a wall of test items whose front and spine layers each hold a solid color of their own (probe_layout.hpp), drawn
// with the renderer of rv_app. The screenshots are probed at every quad's center: in the base color inspection view
// each item shows exactly its own pictures, in the lit view its pictures as the uniform ambient lights them and the
// display transform shows them. The runs cover 2,049 items (two arrays per class: four layered batches and the shells'
// one), arrays grown from 64 to 192 layers mid-run (the pictures uploaded before the growth kept by the GPU copy), the
// RGBA8 fallback
// (--no-texture-compression) and BC1, the GPU memory the arrays record and the uploads' CPU section.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/core/cpu_timers.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_array_layout.hpp>
#include <rv/render/tonemap.hpp>

#include "image_checks.hpp"
#include "probe_layout.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace probe = rv::probe;
namespace gl = rv::gl;

namespace {

std::filesystem::path probeExe() {
    return std::filesystem::path(RV_SMOKE_COLLECTION_PROBE);
}

constexpr glm::ivec2 kShotSize{1920, 1080};
/// Largest channel difference between a probed pixel and its picture in the base color view: the sRGB decode of the
/// texture and the encode of the output each round once.
constexpr int kExactTolerance = 2;

/// One run of the probe, its screenshot read back.
struct ProbeRun {
    AppRun run;
    rv::assets::ImageData image;
};

ProbeRun runProbe(const std::filesystem::path& caseDir, std::string_view name, probe::Scenario scenario,
                  std::string_view view, bool compressed, std::uint64_t frames) {
    const std::filesystem::path shot = caseDir / fmt::format("{}.png", name);
    std::vector<std::string> arguments{"--probe-scenario",
                                       std::string(probe::toString(scenario)),
                                       "--probe-view",
                                       std::string(view),
                                       "--exit-after-frames",
                                       "--frames",
                                       std::to_string(frames),
                                       "--fixed-dt",
                                       "0.016667",
                                       "--screenshot",
                                       arg(shot)};
    if (!compressed) {
        arguments.emplace_back("--no-texture-compression");
    }
    ProbeRun result{runApp(probeExe(), caseDir, name, std::move(arguments)), {}};
    skipWithoutOpenGl(result.run);
    requireCleanRun(result.run);
    result.image = requireScreenshot(result.run, shot, kShotSize);
    return result;
}

std::array<int, 3> pixelAt(const rv::assets::ImageData& image, glm::ivec2 pixel) {
    const std::size_t offset = (static_cast<std::size_t>(pixel.y) * static_cast<std::size_t>(image.width) +
                                static_cast<std::size_t>(pixel.x)) *
                               static_cast<std::size_t>(image.channels);
    return {image.pixels[offset], image.pixels[offset + 1], image.pixels[offset + 2]};
}

std::array<int, 3> channels(gl::Rgb8 color) {
    return {color.r, color.g, color.b};
}

int largestDifference(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    int largest = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        largest = std::max(largest, std::abs(a[c] - b[c]));
    }
    return largest;
}

/// One probed quad: the item, its class, where it shows and what it shows.
struct Probe {
    std::size_t item = 0;
    gl::CollectionClass cls = gl::CollectionClass::Front;
    glm::ivec2 pixel{0};
    std::array<int, 3> expected{};
    std::array<int, 3> actual{};
};

/// The front and spine quads of items [first, last) of a scenario's wall in `image`.
std::vector<Probe> probeItems(const rv::assets::ImageData& image, probe::Scenario scenario, std::size_t first,
                              std::size_t last) {
    const probe::Grid grid = probe::gridFor(probe::countsOf(scenario).final);
    const rv::scene::CameraSetup camera = probe::cameraFor(grid);
    std::vector<Probe> probes;
    for (std::size_t item = first; item < last; ++item) {
        for (const gl::CollectionClass cls : gl::kCollectionClasses) {
            const bool front = cls == gl::CollectionClass::Front;
            const std::optional<glm::ivec2> pixel = projectToPixel(
                camera, kShotSize, front ? probe::frontCenter(grid, item) : probe::spineCenter(grid, item));
            REQUIRE(pixel.has_value());
            Probe entry;
            entry.item = item;
            entry.cls = cls;
            entry.pixel = *pixel;
            entry.expected = channels(front ? probe::frontColor(item) : probe::spineColor(item));
            entry.actual = pixelAt(image, *pixel);
            probes.push_back(entry);
        }
    }
    return probes;
}

std::string describe(const Probe& entry) {
    return fmt::format("item {} {} at ({}, {}): expected {},{},{}, got {},{},{}", entry.item, gl::toString(entry.cls),
                       entry.pixel.x, entry.pixel.y, entry.expected[0], entry.expected[1], entry.expected[2],
                       entry.actual[0], entry.actual[1], entry.actual[2]);
}

/// Base color view: every probed quad shows its own picture within kExactTolerance.
void checkExact(const std::vector<Probe>& probes) {
    std::size_t wrong = 0;
    int largest = 0;
    for (const Probe& entry : probes) {
        const int difference = largestDifference(entry.expected, entry.actual);
        largest = std::max(largest, difference);
        if (difference > kExactTolerance) {
            if (wrong < 5) {
                UNSCOPED_INFO(describe(entry));
            }
            ++wrong;
        }
    }
    INFO(probes.size() << " quads probed, largest channel difference " << largest);
    CHECK(wrong == 0);
}

/// Lit view: every item is lit alike by the probe's uniform white ambient (sky and ground alike, intensity 1, no light,
/// no ambient occlusion, exposure 1), so a quad sends back its picture's linear color (the diffuse ambient: albedo
/// times 1) plus one small grey term, the ambient specular of a rough dielectric, nearly the same for every item; the
/// post pass shows that through the display transform (gl::tonemap, the CPU mirror of tonemap.glsl). The grey term is
/// fitted (the value in [0, 0.1] that makes the largest error smallest), then every probed pixel must be its own
/// picture so displayed within kLitTolerance: an item showing another layer would be tens of steps off.
void checkLit(const std::vector<Probe>& probes) {
    constexpr int kLitTolerance = 3;
    const auto largestError = [&probes](float specular, const Probe** worst) {
        int largest = 0;
        for (const Probe& entry : probes) {
            const glm::vec3 picture =
                rv::core::srgbToLinear(glm::vec3(entry.expected[0], entry.expected[1], entry.expected[2]) / 255.0f);
            const glm::vec3 shown = rv::core::linearToSrgb(gl::tonemap(picture + glm::vec3(specular), 1.0f)) * 255.0f;
            const std::array<int, 3> predicted{static_cast<int>(std::lround(shown.x)),
                                               static_cast<int>(std::lround(shown.y)),
                                               static_cast<int>(std::lround(shown.z))};
            const int difference = largestDifference(predicted, entry.actual);
            if (difference > largest) {
                largest = difference;
                if (worst != nullptr) {
                    *worst = &entry;
                }
            }
        }
        return largest;
    };
    float bestSpecular = 0.0f;
    int bestError = 256;
    for (int step = 0; step <= 100; ++step) {
        const float specular = static_cast<float>(step) * 0.001f;
        if (const int error = largestError(specular, nullptr); error < bestError) {
            bestError = error;
            bestSpecular = specular;
        }
    }
    const Probe* worst = nullptr;
    largestError(bestSpecular, &worst);
    INFO(probes.size() << " quads probed; ambient specular " << bestSpecular << ", largest channel difference "
                       << bestError << (worst != nullptr ? " at " + describe(*worst) : std::string()));
    CHECK(bestError <= kLitTolerance);
}

/// The arrays' bytes for `items` items per class in `format` (planArrayLayers, every array recorded).
std::uint64_t arraysBytes(std::size_t items, gl::CollectionArrayFormat format) {
    std::uint64_t bytes = 0;
    for (const gl::CollectionClass cls : gl::kCollectionClasses) {
        for (const int layers : gl::planArrayLayers(items)) {
            bytes += gl::arrayBytes(cls, format, layers);
        }
    }
    return bytes;
}

/// The run's log holds `line`.
void checkLogged(const AppRun& run, const std::string& line) {
    const auto log = rv::core::readTextFile(run.outputFile);
    REQUIRE(log.has_value());
    INFO(line);
    CHECK(log->find(line) != std::string::npos);
}

const rv::app::CpuSectionRecord* findSection(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::CpuSectionRecord& section : report.cpuSections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

/// The uploads went through the per-frame budget: a typical frame well within an acceptable share of the frame (the
/// budget aims at 1 ms; only the median is bounded, loosely, so that a busy machine does not fail the check).
void checkUploadSection(const rv::app::RunReport& report, std::size_t layers) {
    const rv::app::CpuSectionRecord* uploads = findSection(report, rv::core::cpu_section::kLayerUploads);
    REQUIRE(uploads != nullptr);
    INFO("layer uploads: " << uploads->times.count << " frame(s), p50 " << uploads->times.p50Ms << " ms, p99 "
                           << uploads->times.p99Ms << " ms, max " << uploads->times.maxMs << " ms");
    // At most 64 layers a frame, the pending layers excepted (uploaded with their arrays).
    CHECK(uploads->times.count >= (layers + 63) / 64);
    CHECK(uploads->times.p50Ms <= 4.0);
}

} // namespace

TEST_CASE("collection arrays/2,049 items: two arrays per class, every item showing its own front and spine",
          "[gpu][collection_arrays]") {
    constexpr std::size_t kItems = 2049;
    REQUIRE(probe::countsOf(probe::Scenario::Chunks).final == kItems);
    for (const bool compressed : {true, false}) {
        const gl::CollectionArrayFormat format =
            compressed ? gl::CollectionArrayFormat::Bc1Srgb : gl::CollectionArrayFormat::Rgba8Srgb;
        DYNAMIC_SECTION(gl::toString(format)) {
            // A directory per format: a section runs in its own pass of the test case.
            const std::filesystem::path caseDir =
                freshCaseDir(fmt::format("collection_arrays_chunks_{}", gl::toString(format)));
            const ProbeRun base = runProbe(caseDir, fmt::format("base_{}", gl::toString(format)),
                                           probe::Scenario::Chunks, "base-color", compressed, 240);
            checkExact(probeItems(base.image, probe::Scenario::Chunks, 0, kItems));

            const ProbeRun lit = runProbe(caseDir, fmt::format("lit_{}", gl::toString(format)), probe::Scenario::Chunks,
                                          "lit", compressed, 240);
            checkLit(probeItems(lit.image, probe::Scenario::Chunks, 0, kItems));

            const rv::app::RunReport& report = *lit.run.report;
            INFO(lit.run.describe());
            // The backdrop, the shells, and the fronts and spines in two batches each: one per array.
            REQUIRE(report.lastView.has_value());
            CHECK(report.lastView->items == 1 + 3 * kItems);
            CHECK(report.lastView->culledItems == 0);
            CHECK(report.lastView->batches == 6);
            CHECK(report.lastView->drawCalls == 6);
            CHECK(report.lastView->instances == 1 + 3 * kItems);
            // Two arrays per class, 2,048 and 64 layers, exactly their computed bytes.
            CHECK(gl::planArrayLayers(kItems) == std::vector<int>{2048, 64});
            const gl::GpuMemoryUsage& arrays = report.gpuMemory.usage(gl::GpuMemoryKind::CollectionArrays);
            CHECK(arrays.bytes == arraysBytes(kItems, format));
            CHECK(arrays.peakBytes == arrays.bytes);
            CHECK(arrays.allocations == 4);
            checkUploadSection(report, 2 * kItems);
        }
    }
}

TEST_CASE("collection arrays/growth: arrays of 64 layers grown to 192 keep every resident layer",
          "[gpu][collection_arrays]") {
    const probe::ScenarioCounts counts = probe::countsOf(probe::Scenario::Growth);
    REQUIRE(gl::planArrayLayers(counts.initial) == std::vector<int>{64});
    REQUIRE(gl::planArrayLayers(counts.final) == std::vector<int>{192});
    for (const bool compressed : {true, false}) {
        const gl::CollectionArrayFormat format =
            compressed ? gl::CollectionArrayFormat::Bc1Srgb : gl::CollectionArrayFormat::Rgba8Srgb;
        DYNAMIC_SECTION(gl::toString(format)) {
            const std::filesystem::path caseDir =
                freshCaseDir(fmt::format("collection_arrays_growth_{}", gl::toString(format)));
            // Before: the last frame comes before the growth; the first items show their pictures, the others are not
            // placed yet (the backdrop shows in their cells).
            const ProbeRun before = runProbe(caseDir, fmt::format("before_{}", gl::toString(format)),
                                             probe::Scenario::Growth, "base-color", compressed, probe::kGrowthFrame);
            checkExact(probeItems(before.image, probe::Scenario::Growth, 0, counts.initial));
            std::size_t shown = 0;
            for (const Probe& entry : probeItems(before.image, probe::Scenario::Growth, counts.initial, counts.final)) {
                shown += largestDifference(entry.expected, entry.actual) <= kExactTolerance ? 1 : 0;
            }
            CHECK(shown == 0);
            const gl::GpuMemoryUsage& small = before.run.report->gpuMemory.usage(gl::GpuMemoryKind::CollectionArrays);
            CHECK(small.bytes == arraysBytes(counts.initial, format));
            CHECK(small.allocations == 2);
            checkLogged(before.run, fmt::format("every layer of the {} item(s) is resident ({} uploaded)",
                                                counts.initial, 2 * counts.initial));

            // After: every item shows its pictures, the first ones copied by the growth and never uploaded again.
            const ProbeRun after = runProbe(caseDir, fmt::format("after_{}", gl::toString(format)),
                                            probe::Scenario::Growth, "base-color", compressed, 60);
            checkExact(probeItems(after.image, probe::Scenario::Growth, 0, counts.final));
            const rv::app::RunReport& report = *after.run.report;
            const gl::GpuMemoryUsage& grown = report.gpuMemory.usage(gl::GpuMemoryKind::CollectionArrays);
            CHECK(grown.bytes == arraysBytes(counts.final, format));
            CHECK(grown.allocations == 2);
            // The fronts grow first, then the spines: each new array is recorded before the old one goes.
            const std::uint64_t front64 = gl::arrayBytes(gl::CollectionClass::Front, format, 64);
            const std::uint64_t spine64 = gl::arrayBytes(gl::CollectionClass::Spine, format, 64);
            const std::uint64_t front192 = gl::arrayBytes(gl::CollectionClass::Front, format, 192);
            const std::uint64_t spine192 = gl::arrayBytes(gl::CollectionClass::Spine, format, 192);
            CHECK(grown.peakBytes == std::max(front64 + spine64 + front192, front192 + spine64 + spine192));
            // Each layer uploaded once: the first items' before the growth, the others' after it.
            checkLogged(after.run, "the arrays grown");
            checkLogged(after.run, fmt::format("every layer of the {} item(s) is resident ({} uploaded)", counts.final,
                                               2 * counts.final));
            checkUploadSection(report, 2 * counts.final);

            const ProbeRun lit = runProbe(caseDir, fmt::format("after_lit_{}", gl::toString(format)),
                                          probe::Scenario::Growth, "lit", compressed, 60);
            checkLit(probeItems(lit.image, probe::Scenario::Growth, 0, counts.final));
        }
    }
}
