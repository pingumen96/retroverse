// The collection's texture arrays as data (I3 step 6, ADR-0011 §4 and §6): the classes' sizes and bytes, the format and
// its fallback, how arrays are sized and split, the layer images, the layers' owners with the least recently released
// reused, and the per-frame upload budget.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_array_layout.hpp>

using rv::gl::CollectionArrayFormat;
using rv::gl::CollectionClass;
using rv::gl::LayerAllocator;
using rv::gl::LayerImage;
using rv::gl::Rgb8;
using rv::gl::UploadBudget;
using rv::gl::UploadBudgetMeter;
using namespace std::chrono_literals;

namespace {

constexpr CollectionArrayFormat kBc1 = CollectionArrayFormat::Bc1Srgb;
constexpr CollectionArrayFormat kRgba8 = CollectionArrayFormat::Rgba8Srgb;
constexpr CollectionClass kFront = CollectionClass::Front;
constexpr CollectionClass kSpine = CollectionClass::Spine;

/// The 8-bit color of texel (x, y) of a BC1 level `width` wide (blocks row by row): endpoint 0 or 1, or their
/// interpolations, as the decoder of the S3TC specification computes them (bit replication of the endpoints).
Rgb8 decodeBc1Texel(const std::vector<std::byte>& level, int width, int x, int y) {
    const int blocksWide = (width + 3) / 4;
    const std::size_t offset =
        (static_cast<std::size_t>(y / 4) * static_cast<std::size_t>(blocksWide) + static_cast<std::size_t>(x / 4)) * 8;
    const auto byte = [&](std::size_t index) {
        return std::to_integer<std::uint32_t>(level[offset + index]);
    };
    const auto c0 = static_cast<std::uint16_t>(byte(0) | (byte(1) << 8));
    const auto c1 = static_cast<std::uint16_t>(byte(2) | (byte(3) << 8));
    const std::uint32_t indices = byte(4) | (byte(5) << 8) | (byte(6) << 16) | (byte(7) << 24);
    const std::uint32_t index = (indices >> (2 * ((y % 4) * 4 + (x % 4)))) & 3u;
    const Rgb8 e0 = rv::gl::fromRgb565(c0);
    const Rgb8 e1 = rv::gl::fromRgb565(c1);
    if (index == 0) {
        return e0;
    }
    if (index == 1) {
        return e1;
    }
    const auto mix = [&](int a, int b) {
        return static_cast<std::uint8_t>(c0 > c1 ? (index == 2 ? (2 * a + b) / 3 : (a + 2 * b) / 3) : (a + b) / 2);
    };
    return {mix(e0.r, e1.r), mix(e0.g, e1.g), mix(e0.b, e1.b)};
}

} // namespace

TEST_CASE("render/texture_array_layout: the classes' layer sizes, levels and bytes", "[render][texture_array]") {
    CHECK(rv::gl::layerSize(kFront, kBc1) == glm::ivec2(256, 188));
    CHECK(rv::gl::layerSize(kSpine, kBc1) == glm::ivec2(40, 256));
    // The fallback: RGBA8 at half the resolution.
    CHECK(rv::gl::layerSize(kFront, kRgba8) == glm::ivec2(128, 94));
    CHECK(rv::gl::layerSize(kSpine, kRgba8) == glm::ivec2(20, 128));
    CHECK(rv::gl::layerLevelCount(kFront, kBc1) == 9);
    CHECK(rv::gl::layerLevelCount(kSpine, kBc1) == 9);
    CHECK(rv::gl::layerLevelCount(kFront, kRgba8) == 8);
    CHECK(rv::gl::layerLevelCount(kSpine, kRgba8) == 8);

    CHECK(rv::gl::mipLevelSize({256, 188}, 2) == glm::ivec2(64, 47));
    CHECK(rv::gl::mipLevelSize({256, 188}, 8) == glm::ivec2(1, 1));
    CHECK(rv::gl::mipLevelSize({40, 256}, 6) == glm::ivec2(1, 4));
    CHECK(rv::gl::mipLevelSize({40, 256}, -1) == glm::ivec2(0, 0));

    // BC1: whole 4 x 4 blocks of 8 bytes; 64 x 47 is 16 x 12 blocks, 1 x 1 one block.
    CHECK(rv::gl::layerLevelBytes(kFront, kBc1, 0) == 64 * 47 * 8);
    CHECK(rv::gl::layerLevelBytes(kFront, kBc1, 2) == 16 * 12 * 8);
    CHECK(rv::gl::layerLevelBytes(kFront, kBc1, 8) == 8);
    CHECK(rv::gl::layerLevelBytes(kFront, kBc1, 9) == 0);
    CHECK(rv::gl::layerLevelBytes(kFront, kRgba8, 0) == 128 * 94 * 4);
    CHECK(rv::gl::layerBytes(kFront, kBc1) == 32280);
    CHECK(rv::gl::layerBytes(kSpine, kBc1) == 6984);
    CHECK(rv::gl::layerBytes(kFront, kRgba8) == 64012);
    CHECK(rv::gl::layerBytes(kSpine, kRgba8) == 13628);

    // ADR-0011 §9: 39 KiB per item in BC1, at most 80 MiB for 2,048 layers of each class; RGBA8 at half the size takes
    // about twice that.
    const std::size_t perItem = rv::gl::layerBytes(kFront, kBc1) + rv::gl::layerBytes(kSpine, kBc1);
    CHECK(perItem <= 39 * 1024);
    const std::size_t full = rv::gl::arrayBytes(kFront, kBc1, 2048) + rv::gl::arrayBytes(kSpine, kBc1, 2048);
    CHECK(full == 2048 * perItem);
    CHECK(full <= std::size_t{80} * 1024 * 1024);
    const std::size_t fallback = rv::gl::layerBytes(kFront, kRgba8) + rv::gl::layerBytes(kSpine, kRgba8);
    CHECK(fallback > 19 * perItem / 10);
    CHECK(fallback < 2 * perItem);
    CHECK(rv::gl::arrayBytes(kSpine, kRgba8, 64) ==
          rv::gl::textureBytes(rv::gl::GpuFormat::Srgb8Alpha8, {20, 128}, 8, 64));
    CHECK(rv::gl::gpuFormatOf(kBc1) == rv::gl::GpuFormat::Bc1);
    CHECK(rv::gl::gpuFormatOf(kRgba8) == rv::gl::GpuFormat::Srgb8Alpha8);
    CHECK(rv::gl::toString(kFront) == "front");
    CHECK(rv::gl::toString(kSpine) == "spine");
    CHECK(rv::gl::toString(kBc1) == "bc1-srgb");
    CHECK(rv::gl::toString(kRgba8) == "rgba8-srgb");
}

TEST_CASE("render/texture_array_layout: BC1 with both extensions, RGBA8 without one or when turned off",
          "[render][texture_array]") {
    CHECK(rv::gl::chooseCollectionArrayFormat(true, false).format == kBc1);
    CHECK(rv::gl::chooseCollectionArrayFormat(false, false).format == kRgba8);
    CHECK(rv::gl::chooseCollectionArrayFormat(true, true).format == kRgba8);
    CHECK(rv::gl::chooseCollectionArrayFormat(false, true).format == kRgba8);
    CHECK(rv::gl::chooseCollectionArrayFormat(true, true).reason == "--no-texture-compression");
    CHECK(rv::gl::chooseCollectionArrayFormat(false, true).reason == "--no-texture-compression");
    CHECK(rv::gl::chooseCollectionArrayFormat(false, false).reason.find("missing") != std::string_view::npos);
}

TEST_CASE("render/texture_array_layout: arrays sized to the room, 64 layers at a time, 2,048 at most",
          "[render][texture_array]") {
    // Items plus the pending layer, rounded up to 64.
    CHECK(rv::gl::arrayLayersFor(0) == 64);
    CHECK(rv::gl::arrayLayersFor(1) == 64);
    CHECK(rv::gl::arrayLayersFor(63) == 64);
    CHECK(rv::gl::arrayLayersFor(64) == 128);
    CHECK(rv::gl::arrayLayersFor(191) == 192);
    CHECK(rv::gl::arrayLayersFor(890) == 896);
    CHECK(rv::gl::arrayLayersFor(2047) == 2048);
    CHECK(rv::gl::arrayLayersFor(5000) == 2048);

    using Layers = std::vector<int>;
    CHECK(rv::gl::planArrayLayers(0) == Layers{64});
    CHECK(rv::gl::planArrayLayers(890) == Layers{896});
    CHECK(rv::gl::planArrayLayers(2047) == Layers{2048});
    // One more item than an array holds: a second array, and a second batch per class.
    CHECK(rv::gl::planArrayLayers(2048) == Layers{2048, 64});
    CHECK(rv::gl::planArrayLayers(2049) == Layers{2048, 64});
    CHECK(rv::gl::planArrayLayers(2 * 2047) == Layers{2048, 2048});
    CHECK(rv::gl::planArrayLayers(2 * 2047 + 1) == Layers{2048, 2048, 64});
    CHECK(rv::gl::kPendingLayer == 0);
    CHECK(rv::gl::kMaxItemsPerArray == 2047);
}

TEST_CASE("render/texture_array_layout: RGB565 endpoints round and replicate their bits", "[render][texture_array]") {
    // Every 5:6:5 value decodes to an 8-bit color that encodes back to it.
    for (std::uint32_t value = 0; value <= 0xFFFF; ++value) {
        const auto rgb565 = static_cast<std::uint16_t>(value);
        REQUIRE(rv::gl::toRgb565(rv::gl::fromRgb565(rgb565)) == rgb565);
    }
    CHECK(rv::gl::fromRgb565(0xFFFF) == Rgb8{255, 255, 255});
    CHECK(rv::gl::fromRgb565(0xF81F) == Rgb8{255, 0, 255});
    CHECK(rv::gl::toRgb565(Rgb8{255, 0, 255}) == 0xF81F);
    // Rounded to the nearest: 4 of 255 is 0.49 of a 5-bit step, 5 is 0.61.
    CHECK(rv::gl::toRgb565(Rgb8{4, 0, 0}) == 0x0000);
    CHECK(rv::gl::toRgb565(Rgb8{5, 0, 0}) == 0x0800);
}

TEST_CASE("render/texture_array_layout: solid layers decode to their color at every level", "[render][texture_array]") {
    const Rgb8 color{66, 130, 205};
    const Rgb8 exact = rv::gl::fromRgb565(rv::gl::toRgb565(color));
    for (const CollectionClass cls : rv::gl::kCollectionClasses) {
        CAPTURE(rv::gl::toString(cls));
        const LayerImage bc1 = rv::gl::makeSolidLayer(cls, kBc1, color);
        CHECK(rv::gl::validateLayerImage(bc1, cls, kBc1).has_value());
        for (int level = 0; level < rv::gl::layerLevelCount(cls, kBc1); ++level) {
            CAPTURE(level);
            const glm::ivec2 size = rv::gl::mipLevelSize(rv::gl::layerSize(cls, kBc1), level);
            const std::vector<std::byte>& bytes = bc1.levels[static_cast<std::size_t>(level)];
            CHECK(decodeBc1Texel(bytes, size.x, 0, 0) == exact);
            CHECK(decodeBc1Texel(bytes, size.x, size.x - 1, size.y - 1) == exact);
            CHECK(decodeBc1Texel(bytes, size.x, size.x / 2, size.y / 2) == exact);
        }
        const LayerImage rgba8 = rv::gl::makeSolidLayer(cls, kRgba8, color);
        CHECK(rv::gl::validateLayerImage(rgba8, cls, kRgba8).has_value());
        const std::vector<std::byte>& last = rgba8.levels.back();
        REQUIRE(last.size() == 4);
        CHECK(std::to_integer<int>(last[0]) == color.r);
        CHECK(std::to_integer<int>(last[1]) == color.g);
        CHECK(std::to_integer<int>(last[2]) == color.b);
        CHECK(std::to_integer<int>(last[3]) == 255);
    }
}

TEST_CASE("render/texture_array_layout: a layer image must be of the class and format, every level whole",
          "[render][texture_array]") {
    const LayerImage good = rv::gl::makeSolidLayer(kFront, kBc1, Rgb8{10, 20, 30});
    CHECK(rv::gl::validateLayerImage(good, kFront, kBc1).has_value());

    // Another class, another format.
    CHECK_FALSE(rv::gl::validateLayerImage(good, kSpine, kBc1).has_value());
    const auto otherFormat = rv::gl::validateLayerImage(good, kFront, kRgba8);
    REQUIRE_FALSE(otherFormat.has_value());
    CHECK(otherFormat.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(otherFormat.error().message.find("bc1-srgb") != std::string::npos);

    // A missing level, a level short of a block (a 47-texel side must be padded to 48: 12 blocks, not 11).
    LayerImage missing = good;
    missing.levels.pop_back();
    CHECK(rv::gl::validateLayerImage(missing, kFront, kBc1).error().message.find("8 level(s) instead of 9") !=
          std::string::npos);
    LayerImage unpadded = good;
    unpadded.levels[2].resize(16 * 11 * 8);
    const auto shortLevel = rv::gl::validateLayerImage(unpadded, kFront, kBc1);
    REQUIRE_FALSE(shortLevel.has_value());
    CHECK(shortLevel.error().message.find("level 2 (64x47)") != std::string::npos);
}

TEST_CASE("render/texture_array_layout: each key gets a layer, never layer 0, and keeps it",
          "[render][texture_array]") {
    LayerAllocator layers(4);
    CHECK(layers.capacity() == 4);
    CHECK(layers.neverUsedCount() == 3);
    const auto a = layers.acquire("a");
    const auto b = layers.acquire("b");
    const auto c = layers.acquire("c");
    REQUIRE((a && b && c));
    CHECK(a->layer == 1);
    CHECK(b->layer == 2);
    CHECK(c->layer == 3);
    CHECK_FALSE(a->resident);
    CHECK_FALSE(a->reused);
    CHECK(layers.usedCount() == 3);
    // Full: every layer is in use.
    CHECK_FALSE(layers.acquire("d").has_value());
    // A key asking again keeps its layer.
    const auto again = layers.acquire("b");
    REQUIRE(again.has_value());
    CHECK(again->layer == 2);
    CHECK(again->reused);
    CHECK(layers.usedCount() == 3);
    CHECK(layers.layerOf("c") == std::optional<std::uint32_t>(3));
    CHECK_FALSE(layers.layerOf("d").has_value());

    // Resident once its picture arrives for its current assignment.
    CHECK(layers.markResident(b->layer, b->generation));
    CHECK(layers.resident(2));
    CHECK(layers.residentUsedCount() == 1);
    CHECK_FALSE(layers.markResident(0, 0));                        // the pending layer is never an item's
    CHECK_FALSE(layers.markResident(9, 1));                        // out of range
    CHECK_FALSE(layers.markResident(c->layer, c->generation + 1)); // another assignment's upload

    // Growing adds layers never used.
    layers.grow(6);
    CHECK(layers.capacity() == 6);
    CHECK(layers.neverUsedCount() == 2);
    CHECK(layers.acquire("d")->layer == 4);
    layers.grow(2); // never shrinks
    CHECK(layers.capacity() == 6);
}

TEST_CASE("render/texture_array_layout: released layers keep their pictures and the least recently released goes first",
          "[render][texture_array]") {
    LayerAllocator layers(4);
    for (const char* key : {"a", "b", "c"}) {
        const auto acquired = layers.acquire(key);
        REQUIRE(acquired.has_value());
        CHECK(layers.markResident(acquired->layer, acquired->generation));
    }
    CHECK(layers.release("b"));
    CHECK(layers.release("a"));
    CHECK_FALSE(layers.release("a")); // not in use any more
    CHECK_FALSE(layers.release("z"));
    CHECK(layers.usedCount() == 1);
    CHECK(layers.releasedCount() == 2);

    // "a" comes back: its own layer, its picture still there (no upload needed).
    const auto back = layers.acquire("a");
    REQUIRE(back.has_value());
    CHECK(back->layer == 1);
    CHECK(back->resident);
    CHECK(back->reused);
    CHECK(layers.releasedCount() == 1);

    // A new key takes the least recently released layer ("b"'s), which forgets "b" and waits for its own picture.
    CHECK(layers.release("c"));
    const auto d = layers.acquire("d");
    REQUIRE(d.has_value());
    CHECK(d->layer == 2);
    CHECK_FALSE(d->resident);
    CHECK_FALSE(d->reused);
    CHECK_FALSE(layers.layerOf("b").has_value());
    CHECK_FALSE(layers.resident(2));
    // The next one takes "c"'s.
    const auto e = layers.acquire("e");
    REQUIRE(e.has_value());
    CHECK(e->layer == 3);
    CHECK_FALSE(layers.acquire("f").has_value());

    // An upload queued for "b" before its layer was given to "d" is recognized by its generation.
    CHECK(d->generation == 2);
    CHECK_FALSE(layers.markResident(2, 1));
    CHECK(layers.markResident(2, d->generation));
}

TEST_CASE("render/texture_array_layout: never-used layers go before released ones", "[render][texture_array]") {
    LayerAllocator layers(64);
    REQUIRE(layers.acquire("a").has_value());
    CHECK(layers.release("a"));
    // 62 layers were never used: they are taken first, "a"'s picture kept for its return.
    for (int index = 0; index < 62; ++index) {
        const auto acquired = layers.acquire("item" + std::to_string(index));
        REQUIRE(acquired.has_value());
        CHECK(acquired->layer == static_cast<std::uint32_t>(index + 2));
    }
    CHECK(layers.layerOf("a") == std::optional<std::uint32_t>(1));
    CHECK(layers.acquire("late")->layer == 1);
    CHECK_FALSE(layers.layerOf("a").has_value());
}

TEST_CASE("render/texture_array_layout: the upload budget stops before a layer that would not fit",
          "[render][texture_array]") {
    UploadBudgetMeter meter;
    meter.beginFrame(UploadBudget{1ms, 64});
    // The first layer of a frame always goes, whatever it costs.
    CHECK(meter.mayUploadNext(0ns));
    meter.recordLayer(100us);
    CHECK(meter.expectedLayerCost() == 100us);
    CHECK(meter.mayUploadNext(900us)); // 900 + 100 fits
    CHECK_FALSE(meter.mayUploadNext(901us));
    // The expectation follows the measurements by a quarter of each difference.
    meter.recordLayer(300us);
    CHECK(meter.expectedLayerCost() == 150us);
    CHECK(meter.layersThisFrame() == 2);

    // The layer limit.
    meter.beginFrame(UploadBudget{1s, 3});
    CHECK(meter.layersThisFrame() == 0);
    CHECK(meter.expectedLayerCost() == 150us); // carried over from frame to frame
    for (int layer = 0; layer < 3; ++layer) {
        CHECK(meter.mayUploadNext(0ns));
        meter.recordLayer(1us);
    }
    CHECK_FALSE(meter.mayUploadNext(0ns));

    // A layer that alone exceeds the budget still goes first, then nothing more.
    meter.beginFrame(UploadBudget{1ms, 64});
    CHECK(meter.mayUploadNext(0ns));
    meter.recordLayer(5ms);
    CHECK_FALSE(meter.mayUploadNext(5ms));
    // No layer at all with a zero layer budget.
    meter.beginFrame(UploadBudget{1ms, 0});
    CHECK_FALSE(meter.mayUploadNext(0ns));
}
