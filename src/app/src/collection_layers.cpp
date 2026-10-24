#include <rv/app/collection_layers.hpp>

#include <exception>
#include <utility>

#include <rv/catalog/media.hpp>
#include <rv/core/file_io.hpp>

namespace rv::app {

std::string_view toString(FrontSource source) noexcept {
    switch (source) {
    case FrontSource::Cover:
        return "cover";
    case FrontSource::CachedCover:
        return "cached-cover";
    case FrontSource::Label:
        return "label";
    case FrontSource::LabelForCover:
        return "label-for-cover";
    }
    return "unknown";
}

assets::TextureFormat textureFormatOf(gl::CollectionArrayFormat format) noexcept {
    return format == gl::CollectionArrayFormat::Rgba8Srgb ? assets::TextureFormat::Rgba8Srgb
                                                          : assets::TextureFormat::Bc1Srgb;
}

gl::LayerImage toLayerImage(const assets::RvTexture& texture) {
    gl::LayerImage image;
    image.format = texture.format == assets::TextureFormat::Rgba8Srgb ? gl::CollectionArrayFormat::Rgba8Srgb
                                                                      : gl::CollectionArrayFormat::Bc1Srgb;
    image.levels.reserve(texture.levels.size());
    for (const assets::TextureLevel& level : texture.levels) {
        image.levels.push_back(level.data);
    }
    return image;
}

ItemLayers serveItemLayers(assets::DerivedTextureCache& cache, const assets::LabelFonts& fonts,
                           gl::CollectionArrayFormat format, const std::optional<std::filesystem::path>& mediaRoot,
                           const CoverItem& item) {
    ItemLayers layers;
    try {
        const assets::TextureFormat textureFormat = textureFormatOf(format);
        std::optional<assets::Rgb8> coverColour;
        if (!item.coverPath.empty()) {
            const std::string_view kind = catalog::toString(catalog::MediaKind::Cover);
            const assets::CacheKey key =
                assets::coverCacheKey(assets::TextureClass::Front, textureFormat, kind, item.coverPath, item.label);
            std::optional<std::filesystem::path> source;
            if (mediaRoot) {
                if (auto below = core::extendedLengthPathBelow(*mediaRoot, item.coverPath)) {
                    source = std::move(*below);
                }
            }
            Expected<assets::ServedLayer> served =
                makeError(ErrorCode::Unavailable, "the media root is not known", item.game.toString());
            if (source) {
                assets::FitOptions options;
                options.label = item.label;
                served = assets::serveCover(cache, textureFormat, kind, item.coverPath, *source, options);
            } else if (assets::Lookup found = cache.lookup(key, std::nullopt, true);
                       found.outcome == assets::LookupOutcome::Hit && found.texture) {
                // No media root to read it under: the cover's entry is found by its name and served as it was.
                served =
                    assets::ServedLayer{std::move(*found.texture), assets::ServeOutcome::CacheHitSourceUnavailable};
            }
            if (served) {
                layers.front = toLayerImage(served->texture);
                layers.frontOutcome = served->outcome;
                layers.frontSource = served->outcome == assets::ServeOutcome::CacheHitSourceUnavailable
                                         ? FrontSource::CachedCover
                                         : FrontSource::Cover;
                layers.frontEntry = key.hex();
                coverColour = served->texture.dominant;
            } else {
                layers.coverError = served.error();
                layers.frontSource = FrontSource::LabelForCover;
            }
        }
        if (fonts.title == nullptr || fonts.detail == nullptr) {
            // Without the bundled fonts no layer is generated: the front stays the cover or pending, the spine pending.
            if (!layers.front) {
                layers.error = Error{ErrorCode::Unavailable, "the label fonts are not loaded", item.game.toString()};
            }
            return layers;
        }
        if (!layers.front) {
            assets::FrontLabelRequest request;
            request.title = item.title;
            request.fallbackTitle = item.fallbackTitle;
            request.systemName = item.systemName;
            request.year = item.year;
            request.seed = item.seed;
            request.palette = item.palette.view();
            request.label = item.label;
            if (auto label = assets::serveFrontLabel(cache, fonts, request, textureFormat)) {
                layers.front = toLayerImage(label->texture);
                layers.frontOutcome = label->outcome;
                layers.frontEntry = assets::frontLabelCacheKey(request, textureFormat, fonts).hex();
            } else {
                layers.error = label.error();
            }
        }
        assets::SpineRequest spine;
        spine.title = item.title;
        spine.fallbackTitle = item.fallbackTitle;
        spine.coverColour = coverColour;
        spine.seed = item.seed;
        spine.palette = item.palette.view();
        if (auto served = assets::serveSpine(cache, fonts, spine, textureFormat)) {
            layers.spine = toLayerImage(served->texture);
            layers.spineOutcome = served->outcome;
            layers.spineEntry = assets::spineCacheKey(spine, textureFormat, fonts).hex();
        } else if (!layers.error) {
            layers.error = served.error();
        }
    } catch (const std::exception& error) {
        layers.error = Error{ErrorCode::Unknown, std::string("the item's layers could not be made: ") + error.what(),
                             item.game.toString()};
    }
    return layers;
}

} // namespace rv::app
