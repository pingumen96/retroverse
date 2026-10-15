#include <rv/devui/texture_previews.hpp>

#include <algorithm>

#include <imgui.h>

#include <rv/devui/widgets.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_cache.hpp>

namespace rv::devui {
namespace {

/// Side of the enlarged view in the tooltip, in font sizes.
constexpr float kEnlargedEms = 16.0f;

/// The size of `texture` fitted into a `side` square, keeping its aspect.
ImVec2 fitted(const gl::Texture2D& texture, float side) noexcept {
    const glm::ivec2 size = texture.size();
    if (size.x <= 0 || size.y <= 0) {
        return ImVec2(side, side);
    }
    const float scale = side / static_cast<float>(std::max(size.x, size.y));
    return ImVec2(scale * static_cast<float>(size.x), scale * static_cast<float>(size.y));
}

} // namespace

void TexturePreviews::thumbnail(const gl::Texture2D& texture, float side, std::string_view label) {
    const unsigned int id = displayId(texture);
    if (id == 0) {
        ImGui::TextDisabled("(no preview)");
        return;
    }
    const ImTextureRef image(static_cast<ImTextureID>(id));
    ImGui::Image(image, fitted(texture, side));
    if (ImGui::BeginItemTooltip()) {
        ImGui::Text("%.*s", static_cast<int>(label.size()), label.data());
        const glm::ivec2 size = texture.size();
        ImGui::TextDisabled("%d x %d, %s, %d level(s), ", size.x, size.y,
                            texture.desc().colorSpace == gl::ColorSpace::Srgb ? "sRGB" : "linear", texture.mipLevels());
        ImGui::SameLine(0.0f, 0.0f);
        widgets::bytes(texture.estimatedBytes());
        ImGui::Image(image, fitted(texture, kEnlargedEms * ImGui::GetFontSize()));
        ImGui::EndTooltip();
    }
}

void TexturePreviews::sync() {
    if (m_cache.revision() != m_revision) {
        m_views.clear(); // the textures they showed are gone
        m_revision = m_cache.revision();
    }
}

unsigned int TexturePreviews::displayId(const gl::Texture2D& texture) {
    sync();
    if (!texture.valid()) {
        return 0;
    }
    if (texture.desc().colorSpace != gl::ColorSpace::Srgb) {
        return texture.id();
    }
    auto view = m_views.find(&texture);
    if (view == m_views.end()) {
        view = m_views.emplace(&texture, gl::createRgba8View(texture.id(), texture.mipLevels())).first;
    }
    return view->second.id();
}

} // namespace rv::devui
