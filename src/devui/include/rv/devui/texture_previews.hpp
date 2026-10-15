#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include <rv/render/gl_object.hpp>

namespace rv::gl {
class Texture2D;
class TextureCache;
} // namespace rv::gl

namespace rv::devui {

/// Shows GPU textures in the panels of the developer overlay (Dear ImGui images): thumbnails with an enlarged view and
/// the texture's size, color space and memory in their tooltip. The overlay draws without sRGB encoding, so an sRGB
/// texture is shown through a GL_RGBA8 view of its storage (gl::createRgba8View), whose texels read as the stored
/// bytes: its thumbnail shows the colors it holds; linear textures (normal, metallic-roughness maps) are shown as they
/// are. The views are kept per texture and all dropped when the texture cache releases its content
/// (TextureCache::revision()): a view shares its texture's storage, which it would otherwise keep allocated. Inside an
/// overlay frame only (main thread, GL context current).
class TexturePreviews {
public:
    /// `cache` owns the textures shown (TextureDefaults' textures may be shown too: they outlive it) and must outlive
    /// the previews.
    explicit TexturePreviews(const gl::TextureCache& cache) noexcept : m_cache(cache) {}

    /// Draws `texture` as a `side` x `side` thumbnail (panel units; the texture keeps its aspect inside it), with a
    /// tooltip headed `label` that shows it enlarged.
    void thumbnail(const gl::Texture2D& texture, float side, std::string_view label);
    /// Drops the views when the cache released its content since they were made. thumbnail() does it first; a panel
    /// also calls it from Panel::update(), so that the views of a tab not shown release their storage at once.
    void sync();

private:
    /// The GL texture to show `texture` through: a view for sRGB textures, else the texture itself.
    [[nodiscard]] unsigned int displayId(const gl::Texture2D& texture);

    const gl::TextureCache& m_cache;
    std::uint64_t m_revision = 0;
    std::map<const gl::Texture2D*, gl::TextureObject> m_views;
};

} // namespace rv::devui
