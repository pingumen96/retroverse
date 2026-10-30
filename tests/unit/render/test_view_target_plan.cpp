#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string_view>
#include <vector>

#include <rv/render/gpu_memory.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/ssao_settings.hpp>
#include <rv/render/view_target_plan.hpp>

using rv::gl::GpuFormat;
using rv::gl::ViewImage;
using rv::gl::ViewImagePlan;
using rv::gl::ViewPass;

namespace {

const std::vector<glm::ivec2> kViewSizes{glm::ivec2(1920, 1080), glm::ivec2(1280, 720), glm::ivec2(2561, 1441),
                                         glm::ivec2(1279, 719),  glm::ivec2(3, 5),      glm::ivec2(1, 1)};

/// True when two images that share storage live through it one after the other: `later` from a pass after `earlier`'s
/// last, or from that very pass when it is written in place over `earlier`, the image whose storage it takes.
bool handsOver(const ViewImagePlan& earlier, const ViewImagePlan& later) {
    if (later.firstPass > earlier.lastPass) {
        return true;
    }
    return later.firstPass == earlier.lastPass && later.writtenInPlace && later.storageOf == earlier.image;
}

} // namespace

TEST_CASE("render/view_target_plan: every image of an output once, in order, each living through its passes",
          "[render][view_targets]") {
    const auto plan = rv::gl::viewTargetPlan();
    std::set<std::string_view> names;
    for (std::size_t index = 0; index < plan.size(); ++index) {
        const ViewImagePlan& image = plan[index];
        CAPTURE(image.name);
        CHECK(static_cast<std::size_t>(image.image) == index);
        CHECK(&rv::gl::viewImagePlan(image.image) == &image);
        CHECK(names.insert(image.name).second);
        CHECK(image.firstPass <= image.lastPass);
    }
    // The bloom levels follow the chain's sizes; the raw occlusion has half the output's.
    for (const glm::ivec2 size : kViewSizes) {
        const auto levels = rv::gl::bloomLevelSizes(size);
        for (std::size_t level = 0; level < rv::gl::kBloomLevelCount; ++level) {
            const auto image = static_cast<ViewImage>(static_cast<std::size_t>(ViewImage::BloomLevel0) + level);
            CHECK(rv::gl::viewImageSize(rv::gl::viewImagePlan(image), size) == levels[level]);
        }
        CHECK(rv::gl::viewImageSize(rv::gl::viewImagePlan(ViewImage::AmbientOcclusionRaw), size) ==
              rv::gl::ssaoTargetSize(size));
        CHECK(rv::gl::viewImageSize(rv::gl::viewImagePlan(ViewImage::Output), size) == size);
    }
    CHECK(rv::gl::viewImageSize(rv::gl::viewImagePlan(ViewImage::Output), glm::ivec2(0, -4)) == glm::ivec2(1, 1));
}

TEST_CASE("render/view_target_plan: an image takes over storage only once its owner is done, at its size and class",
          "[render][view_targets]") {
    const auto plan = rv::gl::viewTargetPlan();
    for (const ViewImagePlan& image : plan) {
        if (!image.storageOf) {
            CHECK_FALSE(image.writtenInPlace);
            continue;
        }
        CAPTURE(image.name);
        const ViewImagePlan& owner = rv::gl::viewImagePlan(*image.storageOf);
        // One step: the owner has storage of its own, made before the image (ViewTargets makes them in order).
        CHECK_FALSE(owner.storageOf.has_value());
        CHECK(owner.image < image.image);
        CHECK(rv::gl::sameViewClass(image.format, owner.format));
        for (const glm::ivec2 size : kViewSizes) {
            CHECK(rv::gl::viewImageSize(image, size) == rv::gl::viewImageSize(owner, size));
        }
        // Every pair of images on one storage lives through it one after the other.
        for (const ViewImagePlan& other : plan) {
            if (&other == &image || (other.image != owner.image && other.storageOf != owner.image)) {
                continue;
            }
            CAPTURE(other.name);
            CHECK((handsOver(other, image) || handsOver(image, other)));
        }
    }
}

TEST_CASE("render/view_target_plan: the views of one storage share a texel size", "[render][view_targets]") {
    CHECK(rv::gl::sameViewClass(GpuFormat::Srgb8Alpha8, GpuFormat::Rgba8));
    CHECK(rv::gl::sameViewClass(GpuFormat::Srgb8Alpha8, GpuFormat::R11fG11fB10f));
    CHECK(rv::gl::sameViewClass(GpuFormat::Rg16F, GpuFormat::R11fG11fB10f));
    CHECK(rv::gl::sameViewClass(GpuFormat::Rgba16F, GpuFormat::Rgba16F));
    CHECK_FALSE(rv::gl::sameViewClass(GpuFormat::Rgba16F, GpuFormat::Srgb8Alpha8));
    CHECK_FALSE(rv::gl::sameViewClass(GpuFormat::R8, GpuFormat::Rg8));
    CHECK_FALSE(rv::gl::sameViewClass(GpuFormat::Depth24Stencil8, GpuFormat::Depth24Stencil8));
    CHECK_FALSE(rv::gl::sameViewClass(GpuFormat::Depth32F, GpuFormat::Srgb8Alpha8));
    CHECK_FALSE(rv::gl::sameViewClass(GpuFormat::Bc1, GpuFormat::Bc1));
}

TEST_CASE("render/view_target_plan: the targets' bytes at 720p and 1080p", "[render][view_targets]") {
    // 1080p: the HDR target (radiance RGBA16F, emission R11F_G11F_B10F, depth/stencil), the occlusion at half and full
    // resolution, five R11F_G11F_B10F bloom levels (the first takes the half-resolution occlusion's storage) and the
    // output (no depth of its own); the tonemapped image takes the emission's storage.
    constexpr std::size_t kPixels1080 = 1920u * 1080u;
    constexpr std::size_t kBloom1080 = (480u * 270u + 240u * 135u + 120u * 68u + 60u * 34u + 30u * 17u) * 4u;
    CHECK(rv::gl::viewTargetsBytes(glm::ivec2(1920, 1080)) ==
          kPixels1080 * (8 + 4 + 4) + 960u * 540u * 4u + kPixels1080 + kBloom1080 + kPixels1080 * 4);
    CHECK(rv::gl::viewTargetsBytes(glm::ivec2(1920, 1080)) == 46310040u); // 44.2 MiB (72.5 before the margin)
    CHECK(rv::gl::viewTargetsBytes(glm::ivec2(1280, 720)) == 20582240u);  // 19.6 MiB (32.2)
    // Every image with storage of its own counts, at its size.
    std::size_t bytes = 0;
    for (const ViewImagePlan& image : rv::gl::viewTargetPlan()) {
        if (!image.storageOf) {
            bytes += rv::gl::textureBytes(image.format, rv::gl::viewImageSize(image, glm::ivec2(2561, 1441)), 1);
        }
    }
    CHECK(rv::gl::viewTargetsBytes(glm::ivec2(2561, 1441)) == bytes);
}
