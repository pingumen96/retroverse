#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_defaults.hpp>

namespace rv::gl {

/// Uploads each texture source once and hands out the GPU texture for it. Sources and their keys:
///  - asset files: "<asset-relative path>|<color space>", decoded with assets::loadImage, repeating (tiling surfaces);
///  - procedural patterns: "pattern:<name>:<title>:<seed>@<width>x<height>|<color space>" (assets::patternKey; the
///    size is part of the key because patterns are generated at the aspect ratio of the surface that shows them),
///    rendered with assets::makeNamedPattern, clamped to their edges (single pictures);
///  - decoded images, such as the images of a glTF model: "<caller key>|<color space>".
/// Every texture has the full mip chain and the cache's anisotropy. A source that cannot be loaded or uploaded is
/// logged as a warning once, and its key then always gives TextureDefaults::checker() (style §21: missing assets
/// are visible, never fatal). Returned references stay valid as long as the cache (and the defaults) are alive.
/// Main thread only.
class TextureCache {
public:
    /// `assetRoot` resolves file paths; `defaults` must outlive the cache; `anisotropy` is already clamped to the
    /// device limit (clampAnisotropy).
    TextureCache(std::filesystem::path assetRoot, const TextureDefaults& defaults, float anisotropy);

    /// Texture of an image file under the asset root. `assetPath` is asset-relative with forward slashes
    /// (assets::TextureRef::path, scene texture files); absolute paths and ".." components are refused.
    [[nodiscard]] const Texture2D& file(std::string_view assetPath, ColorSpace colorSpace);
    /// Texture of the procedural pattern `name` (assets::patternNames) with `title` and `seed`, generated at `size`
    /// pixels (assets::patternSizeForAspect).
    [[nodiscard]] const Texture2D& pattern(std::string_view name, std::string_view title, std::uint32_t seed,
                                           glm::ivec2 size, ColorSpace colorSpace);
    /// Texture of a decoded image; `key` identifies the image among the cache's sources (for instance
    /// "<model path>#image<index>"), `wrap` is how the image's surfaces map it. file() and pattern() upload through
    /// it once their image is decoded or generated.
    [[nodiscard]] const Texture2D& image(std::string_view key, const assets::ImageData& image, ColorSpace colorSpace,
                                         TextureWrap wrap);

    /// Textures uploaded so far (failed sources excluded).
    [[nodiscard]] std::size_t textureCount() const noexcept;
    /// Sources that fell back to the checker.
    [[nodiscard]] std::size_t failedCount() const noexcept;
    /// GPU memory of the uploaded textures, mip chains included.
    [[nodiscard]] std::size_t estimatedBytes() const noexcept;
    /// Anisotropy of the cached textures, for samplers that should filter them alike.
    [[nodiscard]] float anisotropy() const noexcept { return m_anisotropy; }

private:
    /// The texture of `key` when it was requested before (the checker for a source that failed), nullptr otherwise.
    [[nodiscard]] const Texture2D* lookup(std::string_view key) const;
    /// Uploads `image` with the cache's sampling, labeled after `key`.
    [[nodiscard]] Expected<Texture2D> upload(const std::string& key, const assets::ImageData& image,
                                             ColorSpace colorSpace, TextureWrap wrap) const;
    /// Records the outcome for `key` (a failure is logged once) and returns the texture to use.
    const Texture2D& insert(const std::string& key, Expected<Texture2D> texture);

    std::filesystem::path m_assetRoot;
    const TextureDefaults& m_defaults;
    float m_anisotropy;
    /// Empty optional: the source failed and is served by the checker. std::map keeps references stable.
    std::map<std::string, std::optional<Texture2D>, std::less<>> m_textures;
};

} // namespace rv::gl
