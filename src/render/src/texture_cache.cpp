#include <rv/render/texture_cache.hpp>

#include <chrono>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/assets/image_io.hpp>
#include <rv/assets/surface_textures.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/text.hpp>

namespace rv::gl {
namespace {

/// Cache key of `source` sampled in `colorSpace` with `wrap`.
std::string cacheKey(std::string_view source, ColorSpace colorSpace, TextureWrap wrap) {
    return fmt::format("{}|{}|{}", source, toString(colorSpace), toString(wrap));
}

/// Asset-relative path: relative, no root name, no ".." component.
Expected<std::filesystem::path> assetRelativePath(std::string_view assetPath) {
    const std::filesystem::path path = core::pathFromUtf8(assetPath);
    if (assetPath.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return makeError(ErrorCode::InvalidArgument, "texture paths are relative to the asset root",
                         std::string(assetPath));
    }
    for (const std::filesystem::path& component : path) {
        if (component == "..") {
            return makeError(ErrorCode::InvalidArgument, "texture paths must stay inside the asset root",
                             std::string(assetPath));
        }
    }
    return path;
}

} // namespace

TextureCache::TextureCache(std::filesystem::path assetRoot, const TextureDefaults& defaults, float anisotropy)
    : m_assetRoot(std::move(assetRoot)), m_defaults(defaults), m_anisotropy(anisotropy) {}

const Texture2D& TextureCache::file(std::string_view assetPath, ColorSpace colorSpace, TextureWrap wrap) {
    const std::string key = cacheKey(assetPath, colorSpace, wrap);
    if (const Texture2D* cached = lookup(key)) {
        return *cached;
    }
    auto path = assetRelativePath(assetPath);
    if (!path) {
        return insert(key, propagate(path.error()));
    }
    auto decoded = assets::loadImage(m_assetRoot / *path);
    if (!decoded) {
        return insert(key, propagate(decoded.error()));
    }
    return image(assetPath, *decoded, colorSpace, wrap);
}

const Texture2D& TextureCache::pattern(std::string_view name, std::string_view title, std::uint32_t seed,
                                       glm::ivec2 size, ColorSpace colorSpace) {
    constexpr TextureWrap kWrap = TextureWrap::ClampToEdge;
    const std::string source = fmt::format("{}@{}x{}", assets::patternKey(name, title, seed), size.x, size.y);
    const std::string key = cacheKey(source, colorSpace, kWrap);
    if (const Texture2D* cached = lookup(key)) {
        return *cached;
    }
    auto generated = assets::makeNamedPattern(name, size.x, size.y, title, seed);
    if (!generated) {
        return insert(key, propagate(generated.error()));
    }
    return image(source, *generated, colorSpace, kWrap);
}

const Texture2D& TextureCache::procedural(std::string_view name, ColorSpace colorSpace) {
    constexpr TextureWrap kWrap = TextureWrap::Repeat;
    const auto sourceOf = [](std::string_view textureName) {
        return fmt::format("{}{}", assets::kProceduralTexturePrefix, textureName);
    };
    const std::string source = sourceOf(name);
    const std::string key = cacheKey(source, colorSpace, kWrap);
    if (const Texture2D* cached = lookup(key)) {
        return *cached;
    }
    auto parsed = assets::parseProceduralTextureName(name);
    if (!parsed) {
        return insert(key, propagate(parsed.error()));
    }
    const auto start = std::chrono::steady_clock::now();
    auto surface = assets::makeProceduralSurface(parsed->surface);
    if (!surface) {
        return insert(key, propagate(surface.error()));
    }
    spdlog::info("Procedural surface '{}' generated in {:.0f} ms ({}x{})", parsed->surface,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                 surface->baseColor.width, surface->baseColor.height);
    // Generating the surface is the expensive part: every map goes up now, each in its own color space.
    for (const assets::SurfaceMap map :
         {assets::SurfaceMap::BaseColor, assets::SurfaceMap::MetallicRoughness, assets::SurfaceMap::Normal}) {
        if (surface->map(map).isValid()) {
            const ColorSpace space = map == assets::SurfaceMap::BaseColor ? ColorSpace::Srgb : ColorSpace::Linear;
            (void)image(sourceOf(assets::ProceduralTextureName{parsed->surface, map}.toString()), surface->map(map),
                        space, kWrap);
        }
    }
    return image(source, surface->map(parsed->map), colorSpace, kWrap);
}

const Texture2D& TextureCache::image(std::string_view key, const assets::ImageData& image, ColorSpace colorSpace,
                                     TextureWrap wrap) {
    const std::string fullKey = cacheKey(key, colorSpace, wrap);
    if (const Texture2D* cached = lookup(fullKey)) {
        return *cached;
    }
    return insert(fullKey, upload(fullKey, image, colorSpace, wrap));
}

void TextureCache::clearContent() noexcept {
    // The keys of procedural surface maps start with their source, "procedural:<surface>/<map>".
    std::erase_if(m_textures,
                  [](const auto& entry) { return !entry.first.starts_with(assets::kProceduralTexturePrefix); });
    ++m_revision;
}

std::size_t TextureCache::textureCount() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : m_textures) {
        count += entry.second.has_value() ? 1u : 0u;
    }
    return count;
}

std::size_t TextureCache::failedCount() const noexcept {
    return m_textures.size() - textureCount();
}

std::size_t TextureCache::estimatedBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& entry : m_textures) {
        if (entry.second) {
            bytes += entry.second->estimatedBytes();
        }
    }
    return bytes;
}

const Texture2D* TextureCache::lookup(std::string_view key) const {
    const auto found = m_textures.find(key);
    if (found == m_textures.end()) {
        return nullptr;
    }
    return found->second ? &*found->second : &m_defaults.checker();
}

Expected<Texture2D> TextureCache::upload(const std::string& key, const assets::ImageData& image, ColorSpace colorSpace,
                                         TextureWrap wrap) const {
    TextureDesc desc;
    desc.colorSpace = colorSpace;
    desc.sampling.wrap = wrap;
    desc.sampling.anisotropy = m_anisotropy;
    // The keys of procedural surface maps start with their source, "procedural:<surface>/<map>".
    desc.memoryKind = key.starts_with(assets::kProceduralTexturePrefix) ? GpuMemoryKind::ProceduralSurfaces
                                                                        : GpuMemoryKind::SceneTextures;
    return Texture2D::create(image, desc, "Texture " + key);
}

const Texture2D& TextureCache::insert(const std::string& key, Expected<Texture2D> texture) {
    if (!texture) {
        spdlog::warn("Texture '{}' is unavailable and is drawn as a checker: {}", key, texture.error().toString());
        m_textures.emplace(key, std::nullopt);
        return m_defaults.checker();
    }
    const auto inserted = m_textures.emplace(key, std::move(*texture));
    return *inserted.first->second;
}

} // namespace rv::gl
