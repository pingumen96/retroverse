#include <rv/render/view_targets.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <rv/render/uniform_blocks.hpp>

namespace rv::gl {
namespace {

/// The color format of a color image of the plan. The plan gives a color image no other format; one without a
/// framebuffer format asserts and falls back to GL_SRGB8_ALPHA8.
ColorFormat colorFormatFor(GpuFormat format) noexcept {
    switch (format) {
    case GpuFormat::Srgb8Alpha8:
        return ColorFormat::Srgb8Alpha8;
    case GpuFormat::Rgba16F:
        return ColorFormat::Rgba16F;
    case GpuFormat::R8:
        return ColorFormat::R8;
    case GpuFormat::Rg16F:
        return ColorFormat::Rg16F;
    case GpuFormat::R11fG11fB10f:
        return ColorFormat::R11fG11fB10f;
    case GpuFormat::Rgba8:
    case GpuFormat::Rg8:
    case GpuFormat::Depth24Stencil8:
    case GpuFormat::Depth32F:
    case GpuFormat::Bc1:
        break;
    }
    assert(false && "a color image of viewTargetPlan() in a format no framebuffer has");
    return ColorFormat::Srgb8Alpha8;
}

} // namespace

const Framebuffer& ViewTargets::storageOf(ViewImage image, std::size_t& attachment) const noexcept {
    attachment = 0;
    switch (image) {
    case ViewImage::SceneDepth:
    case ViewImage::Radiance:
        return m_hdr;
    case ViewImage::Emission:
        attachment = toIndex(SceneOutput::Emission);
        return m_hdr;
    case ViewImage::AmbientOcclusionRaw:
        return m_ambientOcclusionRaw;
    case ViewImage::AmbientOcclusion:
        return m_ambientOcclusion;
    case ViewImage::Tonemapped:
        return m_tonemapped;
    case ViewImage::Output:
        return m_output;
    default:
        return m_bloom[static_cast<std::size_t>(image) - static_cast<std::size_t>(ViewImage::BloomLevel0)];
    }
}

std::uint64_t ViewTargets::recordedBytes() const noexcept {
    std::uint64_t bytes = m_hdr.recordedBytes() + m_ambientOcclusionRaw.recordedBytes() +
                          m_ambientOcclusion.recordedBytes() + m_tonemapped.recordedBytes() +
                          m_output.recordedBytes() + m_outputWithSceneDepth.recordedBytes();
    for (const Framebuffer& level : m_bloom) {
        bytes += level.recordedBytes();
    }
    return bytes;
}

Expected<Framebuffer> ViewTargets::createImage(ViewImage image, glm::ivec2 size) const {
    const ViewImagePlan& plan = viewImagePlan(image);
    const std::string label = m_debugName + "." + std::string(plan.name);
    if (plan.storageOf) {
        // A view of the storage of an image made before it (the plan's images own storage made in ViewImage order).
        FramebufferAliasDesc alias;
        alias.color = &storageOf(*plan.storageOf, alias.colorAttachment);
        alias.colorFormat = colorFormatFor(plan.format);
        return Framebuffer::createAlias(alias, label);
    }
    // Only the scene's depth has a depth/stencil buffer: the HDR target's (create()).
    return Framebuffer::create(FramebufferDesc{viewImageSize(plan, size), colorFormatFor(plan.format), false}, label);
}

Expected<ViewTargets> ViewTargets::create(glm::ivec2 size, std::string_view debugName) {
    ViewTargets targets;
    targets.m_debugName = std::string(debugName);
    // The HDR target: the radiance, its emitted part and the scene's depth.
    static_assert(kSceneOutputCount == 2, "the HDR target's attachments are the radiance and the emission");
    const ViewImagePlan& radiance = viewImagePlan(ViewImage::Radiance);
    const ViewImagePlan& emission = viewImagePlan(ViewImage::Emission);
    const ViewImagePlan& depth = viewImagePlan(ViewImage::SceneDepth);
    assert(emission.scale == radiance.scale && depth.scale == radiance.scale &&
           "the HDR target's images have one size");
    // A framebuffer's depth/stencil buffer is GL_DEPTH24_STENCIL8, a texture when passes after the one that writes it
    // read it (the ambient occlusion samples it, outputWithSceneDepth() views it).
    assert(depth.format == GpuFormat::Depth24Stencil8 && "the scene's depth is a framebuffer's depth/stencil buffer");
    FramebufferDesc hdrDesc{viewImageSize(radiance, size), colorFormatFor(radiance.format),
                            depth.format == GpuFormat::Depth24Stencil8, kSceneOutputCount,
                            depth.lastPass > depth.firstPass};
    hdrDesc.laterColorFormat = colorFormatFor(emission.format);
    auto hdr = Framebuffer::create(hdrDesc, targets.m_debugName + ".hdr");
    if (!hdr) {
        return propagate(hdr.error());
    }
    targets.m_hdr = std::move(*hdr);

    const auto make = [&targets, size](ViewImage image, Framebuffer& into) -> Status {
        auto created = targets.createImage(image, size);
        if (!created) {
            return propagate(created.error());
        }
        into = std::move(*created);
        return ok();
    };
    Status status = make(ViewImage::AmbientOcclusionRaw, targets.m_ambientOcclusionRaw);
    if (status) {
        status = make(ViewImage::AmbientOcclusion, targets.m_ambientOcclusion);
    }
    for (std::size_t level = 0; status && level < kBloomLevelCount; ++level) {
        status = make(static_cast<ViewImage>(static_cast<std::size_t>(ViewImage::BloomLevel0) + level),
                      targets.m_bloom[level]);
    }
    if (status) {
        status = make(ViewImage::Tonemapped, targets.m_tonemapped);
    }
    if (status) {
        status = make(ViewImage::Output, targets.m_output);
    }
    if (!status) {
        return propagate(status.error());
    }
    // The output over the scene's depth, for the debug lines: storage of others only.
    FramebufferAliasDesc overScene;
    overScene.color = &targets.m_output;
    overScene.colorFormat = targets.m_output.colorFormat();
    overScene.depthStencil = &targets.m_hdr;
    auto withDepth = Framebuffer::createAlias(overScene, targets.m_debugName + ".outputWithSceneDepth");
    if (!withDepth) {
        return propagate(withDepth.error());
    }
    targets.m_outputWithSceneDepth = std::move(*withDepth);
    // What the framebuffers record (the run report's gpuMemory.targets) is what the plan says they allocate.
    assert(targets.recordedBytes() == viewTargetsBytes(size) && "the view targets allocate what viewTargetPlan() says");
    return targets;
}

Status ViewTargets::resize(glm::ivec2 size) {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "cannot resize empty view targets", m_debugName);
    }
    if (size == this->size()) {
        return ok();
    }
    // A complete new set first: on failure the previous targets stay as they are, all of one size.
    auto resized = create(size, m_debugName);
    if (!resized) {
        return propagate(resized.error());
    }
    *this = std::move(*resized);
    return ok();
}

} // namespace rv::gl
