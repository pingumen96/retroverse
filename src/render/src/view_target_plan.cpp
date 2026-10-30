#include <rv/render/view_target_plan.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

#include <rv/render/post_settings.hpp>
#include <rv/render/ssao_settings.hpp>

namespace rv::gl {
namespace {

static_assert(kBloomLevelCount == 6, "one ViewImage per bloom level");

constexpr ViewImagePlan image(ViewImage which, std::string_view name, GpuFormat format, ViewImageScale scale,
                              ViewPass firstPass, ViewPass lastPass, std::size_t bloomLevel = 0,
                              std::optional<ViewImage> storageOf = std::nullopt, bool writtenInPlace = false) {
    return ViewImagePlan{which, name, format, scale, bloomLevel, firstPass, lastPass, storageOf, writtenInPlace};
}

constexpr ViewImagePlan bloomLevel(ViewImage which, std::string_view name, std::size_t level) {
    return image(which, name, GpuFormat::R11fG11fB10f, ViewImageScale::BloomLevel, ViewPass::Bloom, ViewPass::Bloom,
                 level);
}

// The memory margin (docs/i5/memory-margin.md): the output has no depth buffer of its own (the debug lines test and
// write the scene's, the last pass that reads it), the emitted light and the bloom are R11F_G11F_B10F, the tonemapped
// image takes the emitted light's storage (the post pass reads it texel by texel where it writes) and the first bloom
// level the half-resolution occlusion's (over before the bloom starts).
constexpr std::array<ViewImagePlan, kViewImageCount> kPlan{
    image(ViewImage::SceneDepth, "hdr.depthStencil", GpuFormat::Depth24Stencil8, ViewImageScale::Full,
          ViewPass::DepthPrePass, ViewPass::DebugLines),
    image(ViewImage::Radiance, "hdr.color", GpuFormat::Rgba16F, ViewImageScale::Full, ViewPass::DepthPrePass,
          ViewPass::PostPass),
    image(ViewImage::Emission, "hdr.color1", GpuFormat::R11fG11fB10f, ViewImageScale::Full, ViewPass::DepthPrePass,
          ViewPass::PostPass),
    image(ViewImage::AmbientOcclusionRaw, "ssaoRaw", GpuFormat::Rg16F, ViewImageScale::Half,
          ViewPass::AmbientOcclusion, ViewPass::AmbientOcclusionBlur),
    image(ViewImage::AmbientOcclusion, "ssao", GpuFormat::R8, ViewImageScale::Full, ViewPass::AmbientOcclusion,
          ViewPass::LitPass),
    image(ViewImage::BloomLevel0, "bloom0", GpuFormat::R11fG11fB10f, ViewImageScale::BloomLevel, ViewPass::Bloom,
          ViewPass::PostPass, 0, ViewImage::AmbientOcclusionRaw),
    bloomLevel(ViewImage::BloomLevel1, "bloom1", 1),
    bloomLevel(ViewImage::BloomLevel2, "bloom2", 2),
    bloomLevel(ViewImage::BloomLevel3, "bloom3", 3),
    bloomLevel(ViewImage::BloomLevel4, "bloom4", 4),
    bloomLevel(ViewImage::BloomLevel5, "bloom5", 5),
    image(ViewImage::Tonemapped, "tonemapped", GpuFormat::Srgb8Alpha8, ViewImageScale::Full, ViewPass::PostPass,
          ViewPass::AntiAliasing, 0, ViewImage::Emission, true),
    // The post pass writes it when no anti-aliasing follows.
    image(ViewImage::Output, "output", GpuFormat::Srgb8Alpha8, ViewImageScale::Full, ViewPass::PostPass,
          ViewPass::Present),
};

constexpr bool planInOrder() {
    for (std::size_t index = 0; index < kPlan.size(); ++index) {
        if (static_cast<std::size_t>(kPlan[index].image) != index) {
            return false;
        }
    }
    return true;
}
static_assert(planInOrder(), "kPlan lists every ViewImage in its order");

/// Bytes of a texel of an uncompressed color format; 0 for the others.
constexpr std::size_t colorTexelBytes(GpuFormat format) noexcept {
    switch (format) {
    case GpuFormat::R8:
        return 1;
    case GpuFormat::Rg8:
        return 2;
    case GpuFormat::Rgba8:
    case GpuFormat::Srgb8Alpha8:
    case GpuFormat::Rg16F:
    case GpuFormat::R11fG11fB10f:
        return 4;
    case GpuFormat::Rgba16F:
        return 8;
    case GpuFormat::Depth24Stencil8:
    case GpuFormat::Depth32F:
    case GpuFormat::Bc1:
        return 0;
    }
    return 0;
}

} // namespace

std::span<const ViewImagePlan, kViewImageCount> viewTargetPlan() noexcept {
    return kPlan;
}

const ViewImagePlan& viewImagePlan(ViewImage image) noexcept {
    return kPlan[static_cast<std::size_t>(image)];
}

glm::ivec2 viewImageSize(const ViewImagePlan& plan, glm::ivec2 viewSize) noexcept {
    const glm::ivec2 size(std::max(viewSize.x, 1), std::max(viewSize.y, 1));
    switch (plan.scale) {
    case ViewImageScale::Full:
        return size;
    case ViewImageScale::Half:
        return ssaoTargetSize(size);
    case ViewImageScale::BloomLevel:
        return bloomLevelSizes(size)[std::min(plan.bloomLevel, kBloomLevelCount - 1)];
    }
    return size;
}

bool sameViewClass(GpuFormat a, GpuFormat b) noexcept {
    const std::size_t bytes = colorTexelBytes(a);
    return bytes > 0 && bytes == colorTexelBytes(b);
}

std::size_t viewTargetsBytes(glm::ivec2 viewSize) noexcept {
    std::size_t bytes = 0;
    for (const ViewImagePlan& plan : kPlan) {
        if (!plan.storageOf) {
            bytes += textureBytes(plan.format, viewImageSize(plan, viewSize), 1);
        }
    }
    return bytes;
}

} // namespace rv::gl
