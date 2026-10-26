#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

#include <rv/assets/surface_textures.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_memory.hpp>

using rv::gl::GpuFormat;
using rv::gl::GpuMemoryAllocation;
using rv::gl::GpuMemoryKind;
using rv::gl::GpuMemoryLedger;
using rv::gl::GpuMemoryUsage;

TEST_CASE("render/gpu_memory: the bytes of a texture follow its format, size, levels and layers", "[render][gpu_memory]") {
    const glm::ivec2 size(16, 8);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::R8, size) == 128);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Rgba8, size) == 512);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Srgb8Alpha8, size) == 512);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Rg16F, size) == 512);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Depth24Stencil8, size) == 512);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Depth32F, size) == 512);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Rgba16F, size) == 1024);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Rgba8, glm::ivec2(0, 8)) == 0);

    // BC1: 8 bytes per block of 4 x 4, each side rounded up to whole blocks, every level at least one block.
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Bc1, size) == 4 * 2 * 8);
    CHECK(rv::gl::textureLevelBytes(GpuFormat::Bc1, glm::ivec2(5, 1)) == 2 * 1 * 8);
    CHECK(rv::gl::textureBytes(GpuFormat::Bc1, glm::ivec2(8, 8), 4) == (4 + 1 + 1 + 1) * 8);
    // A 256 x 256 layer with its full chain (9 levels): 32 KiB + 8 KiB + 2 KiB + ... + 4 one-block levels.
    const std::size_t layer = rv::gl::textureBytes(GpuFormat::Bc1, glm::ivec2(256), 9);
    CHECK(layer == 32768 + 8192 + 2048 + 512 + 128 + 32 + 8 + 8 + 8);
    CHECK(rv::gl::textureBytes(GpuFormat::Bc1, glm::ivec2(256), 9, 2048) == layer * 2048);

    // RGBA8 agrees with the mip chain the textures have always reported.
    for (const glm::ivec2 extent : {glm::ivec2(1024, 1024), glm::ivec2(1536, 1024), glm::ivec2(300, 17)}) {
        const int levels = rv::gl::fullMipLevelCount(extent);
        CHECK(rv::gl::textureBytes(GpuFormat::Rgba8, extent, levels) == rv::gl::mipChainBytes(extent, levels));
    }
    CHECK(rv::gl::textureBytes(GpuFormat::Rgba16F, glm::ivec2(1920, 1080), 1) == 1920u * 1080u * 8u);
    CHECK(rv::gl::textureBytes(GpuFormat::Rgba8, size, 0) == 0);
    CHECK(rv::gl::textureBytes(GpuFormat::Rgba8, size, 1, 0) == 0);
    CHECK(rv::gl::textureBytes(GpuFormat::Rgba8, glm::ivec2(-1, 4), 1) == 0);
}

TEST_CASE("render/gpu_memory: the procedural surfaces take 123 MiB with their full chains", "[render][gpu_memory]") {
    // The figure ADR-0011 §9 budgets (and the test room's run report shows): every map RGBA8 with its full chain.
    std::size_t bytes = 0;
    for (const rv::assets::ProceduralSurfaceInfo& surface : rv::assets::proceduralSurfaces()) {
        const std::size_t maps = surface.hasNormalMap ? 3 : 2;
        bytes += maps * rv::gl::textureBytes(GpuFormat::Rgba8, surface.size, rv::gl::fullMipLevelCount(surface.size));
    }
    // 36 bytes under 80 MiB: the chains stop at 1 x 1, a little under 4/3 of the base levels. Three 1024 x 1024
    // surfaces with three maps (5,592,404 bytes each), four 512 x 512 ones (1,398,100), the 1536 x 1024 rug with two
    // (8,388,604); since I6 step 2 the 128 x 192 wall wash with two (131,068 each), 256 KiB more; since I6 step 3 the
    // 128 x 64 shelf glow with two (43,692 each); since I6 step 3b four 1152 x 768 rugs with two (4,718,580 each) and
    // the 512 x 1280 runner with two (3,495,252 each).
    CHECK(bytes == std::size_t{83'886'044 + 262'136 + 87'384 + 4 * 2 * 4'718'580 + 2 * 3'495'252});
}

TEST_CASE("render/gpu_memory: kinds have stable names", "[render][gpu_memory]") {
    std::set<std::string_view> names;
    for (const GpuMemoryKind kind : rv::gl::kGpuMemoryKinds) {
        names.insert(rv::gl::toString(kind));
        CHECK(rv::gl::gpuMemoryKindFromString(rv::gl::toString(kind)) == kind);
        CHECK_FALSE(rv::gl::displayName(kind).empty());
        CHECK(static_cast<std::size_t>(kind) < rv::gl::kGpuMemoryKindCount);
    }
    CHECK(names.size() == rv::gl::kGpuMemoryKindCount);
    CHECK(rv::gl::toString(GpuMemoryKind::ProceduralSurfaces) == "proceduralSurfaces");
    CHECK(rv::gl::toString(GpuMemoryKind::CollectionArrays) == "collectionArrays");
    CHECK(rv::gl::toString(GpuMemoryKind::HeroTextures) == "heroTextures");
    CHECK(rv::gl::toString(GpuMemoryKind::Video) == "video");
    CHECK(rv::gl::toString(GpuMemoryKind::Targets) == "targets");
    CHECK_FALSE(rv::gl::gpuMemoryKindFromString("Targets").has_value());
    CHECK_FALSE(rv::gl::gpuMemoryKindFromString("").has_value());
}

TEST_CASE("render/gpu_memory: the ledger counts what lives, per kind, with peaks", "[render][gpu_memory]") {
    GpuMemoryLedger ledger;
    CHECK(ledger.totalBytes() == 0);
    {
        const GpuMemoryAllocation a(GpuMemoryKind::Targets, 100, ledger);
        GpuMemoryAllocation b(GpuMemoryKind::Targets, 50, ledger);
        const GpuMemoryAllocation c(GpuMemoryKind::Video, 7, ledger);
        CHECK(ledger.usage(GpuMemoryKind::Targets) == GpuMemoryUsage{150, 150, 2});
        CHECK(ledger.totalBytes() == 157);
        b.reset();
        CHECK_FALSE(b.recorded());
        b.reset(); // twice: nothing more
        CHECK(ledger.usage(GpuMemoryKind::Targets) == GpuMemoryUsage{100, 150, 1});
        CHECK(ledger.totalBytes() == 107);
        CHECK(ledger.peakTotalBytes() == 157);
    }
    CHECK(ledger.totalBytes() == 0);
    CHECK(ledger.peakTotalBytes() == 157);
    CHECK(ledger.usage(GpuMemoryKind::Video) == GpuMemoryUsage{0, 7, 0});

    // The total's peak is the highest sum, not the sum of the kinds' peaks.
    {
        const GpuMemoryAllocation big(GpuMemoryKind::Buffers, 200, ledger);
    }
    CHECK(ledger.peakTotalBytes() == 200);
    CHECK(ledger.usage(GpuMemoryKind::Buffers).peakBytes == 200);

    // Never below zero.
    ledger.remove(GpuMemoryKind::HeroTextures, 10);
    CHECK(ledger.usage(GpuMemoryKind::HeroTextures) == GpuMemoryUsage{});
    CHECK(ledger.totalBytes() == 0);
}

TEST_CASE("render/gpu_memory: an allocation moves with its owner and can change kind", "[render][gpu_memory]") {
    GpuMemoryLedger ledger;
    GpuMemoryAllocation first(GpuMemoryKind::SceneTextures, 64, ledger);
    GpuMemoryAllocation second(std::move(first));
    CHECK_FALSE(first.recorded()); // NOLINT(bugprone-use-after-move): moved-from is empty by contract
    CHECK(second.recorded());
    CHECK(second.bytes() == 64);
    CHECK(ledger.usage(GpuMemoryKind::SceneTextures) == GpuMemoryUsage{64, 64, 1});

    // Replacing an allocation records the new one before the old is released: both count towards the peak.
    second = GpuMemoryAllocation(GpuMemoryKind::SceneTextures, 32, ledger);
    CHECK(ledger.usage(GpuMemoryKind::SceneTextures) == GpuMemoryUsage{32, 96, 1});

    second.setKind(GpuMemoryKind::HeroTextures);
    CHECK(second.kind() == GpuMemoryKind::HeroTextures);
    CHECK(ledger.usage(GpuMemoryKind::SceneTextures).bytes == 0);
    CHECK(ledger.usage(GpuMemoryKind::HeroTextures) == GpuMemoryUsage{32, 32, 1});
    CHECK(ledger.totalBytes() == 32);

    GpuMemoryAllocation empty;
    empty.setKind(GpuMemoryKind::Video);
    CHECK_FALSE(empty.recorded());
    CHECK(ledger.usage(GpuMemoryKind::Video) == GpuMemoryUsage{});

    // The process's ledger is one.
    CHECK(&rv::gl::gpuMemoryLedger() == &rv::gl::gpuMemoryLedger());
}
