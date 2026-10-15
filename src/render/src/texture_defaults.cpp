#include <rv/render/texture_defaults.hpp>

#include <string_view>
#include <utility>

#include <rv/assets/test_patterns.hpp>

namespace rv::gl {
namespace {

constexpr int kCheckerSize = 64;

Expected<Texture2D> solid(glm::u8vec4 color, std::string_view debugName) {
    TextureDesc desc;
    desc.colorSpace = ColorSpace::Linear;
    desc.mipmaps = false;
    desc.sampling.filter = TextureFilter::Bilinear;
    return Texture2D::create(assets::makeSolid(1, 1, color), desc, debugName);
}

} // namespace

Expected<TextureDefaults> TextureDefaults::create(float anisotropy) {
    TextureDefaults defaults;
    auto white = solid(glm::u8vec4(255, 255, 255, 255), "Texture.DefaultWhite");
    if (!white) {
        return propagate(white.error());
    }
    auto black = solid(glm::u8vec4(0, 0, 0, 255), "Texture.DefaultBlack");
    if (!black) {
        return propagate(black.error());
    }
    auto flatNormal = solid(glm::u8vec4(128, 128, 255, 255), "Texture.DefaultFlatNormal");
    if (!flatNormal) {
        return propagate(flatNormal.error());
    }
    auto checkerImage = assets::makeNamedPattern(assets::kPatternChecker, kCheckerSize, kCheckerSize);
    if (!checkerImage) {
        return propagate(checkerImage.error());
    }
    TextureDesc checkerDesc;
    checkerDesc.colorSpace = ColorSpace::Srgb;
    checkerDesc.sampling.anisotropy = anisotropy;
    auto checker = Texture2D::create(*checkerImage, checkerDesc, "Texture.DefaultChecker");
    if (!checker) {
        return propagate(checker.error());
    }
    defaults.m_white = std::move(*white);
    defaults.m_black = std::move(*black);
    defaults.m_flatNormal = std::move(*flatNormal);
    defaults.m_checker = std::move(*checker);
    return defaults;
}

const Texture2D& TextureDefaults::neutral(MaterialTextureSlot slot) const noexcept {
    switch (slot) {
    case MaterialTextureSlot::Normal:
        return m_flatNormal;
    case MaterialTextureSlot::Emissive:
        return m_black;
    case MaterialTextureSlot::BaseColor:
    case MaterialTextureSlot::MetallicRoughness:
    case MaterialTextureSlot::Occlusion:
        break;
    }
    return m_white;
}

} // namespace rv::gl
