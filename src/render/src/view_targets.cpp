#include <rv/render/view_targets.hpp>

#include <cstddef>
#include <utility>

#include <fmt/format.h>

#include <rv/render/uniform_blocks.hpp>

namespace rv::gl {

Expected<ViewTargets> ViewTargets::create(glm::ivec2 size, std::string_view debugName) {
    ViewTargets targets;
    targets.m_debugName = std::string(debugName);
    auto hdr = Framebuffer::create(FramebufferDesc{size, ColorFormat::Rgba16F, true, kSceneOutputCount},
                                   targets.m_debugName + ".hdr");
    if (!hdr) {
        return propagate(hdr.error());
    }
    targets.m_hdr = std::move(*hdr);
    const std::array<glm::ivec2, kBloomLevelCount> levelSizes = bloomLevelSizes(size);
    for (std::size_t level = 0; level < kBloomLevelCount; ++level) {
        auto bloom = Framebuffer::create(FramebufferDesc{levelSizes[level], ColorFormat::Rgba16F, false},
                                         fmt::format("{}.bloom{}", targets.m_debugName, level));
        if (!bloom) {
            return propagate(bloom.error());
        }
        targets.m_bloom[level] = std::move(*bloom);
    }
    auto output =
        Framebuffer::create(FramebufferDesc{size, ColorFormat::Srgb8Alpha8, true}, targets.m_debugName + ".output");
    if (!output) {
        return propagate(output.error());
    }
    targets.m_output = std::move(*output);
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
