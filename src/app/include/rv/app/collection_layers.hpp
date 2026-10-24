#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// One collection item's two texture layers as the room's loader makes them on the worker pool (I3 step 10b; ADR-0011
// §5 to §7, I3 step 5's pipeline): the front from the cover through the derived-texture cache (assets::serveCover: a
// hit served without reading the cover, a cover that cannot be read served from its entry as it was), else the
// generated front label (no cover, or one that is no usable image), both made for the item's label shape (I4 step 2,
// ADR-0013 §4: CoverItem::label, part of their cache keys); the spine always generated from the title on the
// cover's dominant colour (the palette's colour for the placement's seed without a cover). The layers come out as
// gl::LayerImage of the run's array format, ready for gl::CollectionArrays::submit on the main thread. The user's paths
// are logged at debug level only (the errors carry them).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/collection_labels.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/rvtex.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/texture_array_layout.hpp>

namespace rv::app {

/// What the loader knows of one placed item (from the room's snapshot and plan, world_build.hpp).
struct CoverItem {
    catalog::GameId game;
    /// The catalog's title (the spine's and the label's text) and the title of its file name (when the font has none
    /// of the title's characters).
    std::string title;
    std::string fallbackTitle;
    /// The system's full name and the release year ("" without one): the generated label's bottom line.
    std::string systemName;
    std::string year;
    /// The placement's seed (world::placementSeed): the palette's colour of a spine or label without a cover.
    std::uint64_t seed = 0;
    /// Its room's theme pack's label palette (I6 step 3; empty: assets::kDefaultLabelPalette).
    std::vector<assets::Rgb8> palette;
    /// Its cover's path relative to the media root, as the catalog derives it ("snes/covers/Game (USA).png"); empty
    /// when the catalog records none.
    std::string coverPath;
    /// Its layers' keys in the collection's arrays (world::CollectionItem::frontLayerKey, spineLayerKey).
    std::string frontKey;
    std::string spineKey;
    /// The shape of its room's family's label quad (world::MediaFamily::coverAspect(), ADR-0013 §4): its cover, its
    /// generated label and its hero picture are made for it; the layer's own shape (I3's fit) for the 16-bit family.
    assets::LabelAspect label;

    bool operator==(const CoverItem&) const = default;
};

/// Where an item's front came from.
enum class FrontSource : std::uint8_t {
    Cover,         ///< its cover, read now or from the cache with its source checked
    CachedCover,   ///< its cover from the cache, the cover itself out of reach (the media root unplugged, a file gone)
    Label,         ///< a generated label: the catalog records no cover
    LabelForCover, ///< a generated label: the cover could not be read or is no usable image
};

/// "cover", "cached-cover", "label", "label-for-cover".
[[nodiscard]] std::string_view toString(FrontSource source) noexcept;

/// The two layers of an item, or why one is missing.
struct ItemLayers {
    std::optional<gl::LayerImage> front;
    std::optional<gl::LayerImage> spine;
    FrontSource frontSource = FrontSource::Label;
    /// How each was served (assets::ServeOutcome), when it was.
    std::optional<assets::ServeOutcome> frontOutcome;
    std::optional<assets::ServeOutcome> spineOutcome;
    /// Why the cover gave no front (it may name the user's folders: log it at debug level only).
    std::optional<Error> coverError;
    /// Why a layer could not be made at all (the generated label or the spine failed): that layer stays pending.
    std::optional<Error> error;
    /// The cache entries served for it (their hex names), for the cache's trim (DerivedTextureCache::trim's inUse).
    std::string frontEntry;
    std::string spineEntry;
};

/// The pipeline's format of an array format (BC1 sRGB, RGBA8 sRGB at half the size).
[[nodiscard]] assets::TextureFormat textureFormatOf(gl::CollectionArrayFormat format) noexcept;
/// A layer's levels as the arrays take them (validated by CollectionArrays::submit).
[[nodiscard]] gl::LayerImage toLayerImage(const assets::RvTexture& texture);

/// Serves `item`'s front and spine (see the file comment) from `cache` with `fonts`, in `format`, its cover read under
/// `mediaRoot` (none: the media root is not known, the cache alone serves the cover). Without fonts (either null: the
/// bundled fonts refused) nothing is generated: the front is the cover or nothing, the spine nothing. Never throws;
/// what failed is in the result. Safe on several workers at once (the cache and the fonts are).
[[nodiscard]] ItemLayers serveItemLayers(assets::DerivedTextureCache& cache, const assets::LabelFonts& fonts,
                                         gl::CollectionArrayFormat format,
                                         const std::optional<std::filesystem::path>& mediaRoot, const CoverItem& item);

} // namespace rv::app
