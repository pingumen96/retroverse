#include <rv/render/view_targets.hpp>

#include <cstddef>
#include <utility>

#include <rv/render/uniform_blocks.hpp>

namespace rv::gl {
namespace {

/// The color format of a color image of the plan.
ColorFormat colorFormatOf(GpuFormat format) noexcept {
    switch (format) {
    case GpuFormat::Rgba16F:
        return ColorFormat::Rgba16F;
    case GpuFormat::R8:
        return ColorFormat::R8;
    case GpuFormat::Rg16F:
        return ColorFormat::Rg16F;
    default:
        return ColorFormat::Srgb8Alpha8;
    }
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

Expected<Framebuffer> ViewTargets::createImage(ViewImage image, glm::ivec2 size) const {
    const ViewImagePlan& plan = viewImagePlan(image);
    const std::string label = m_debugName + "." + std::string(plan.name);
    if (plan.storageOf) {
        // A view of the storage of an image made before it (the plan's images own storage made in ViewImage order).
        FramebufferAliasDesc alias;
        alias.color = &storageOf(*plan.storageOf, alias.colorAttachment);
        alias.colorFormat = colorFormatOf(plan.format);
        return Framebuffer::createAlias(alias, label);
    }
    return Framebuffer::create(FramebufferDesc{viewImageSize(plan, size), colorFormatOf(plan.format), false}, label);
}

Expected<ViewTargets> ViewTargets::create(glm::ivec2 size, std::string_view debugName) {
    ViewTargets targets;
    targets.m_debugName = std::string(debugName);
    // The HDR target: the radiance, its emitted part and the scene's depth.
    static_assert(kSceneOutputCount == 2, "the HDR target's attachments are the radiance and the emission");
    const ViewImagePlan& radiance = viewImagePlan(ViewImage::Radiance);
    auto hdr = Framebuffer::create(
        FramebufferDesc{viewImageSize(radiance, size), colorFormatOf(radiance.format), true, kSceneOutputCount, true},
        targets.m_debugName + ".hdr");
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
