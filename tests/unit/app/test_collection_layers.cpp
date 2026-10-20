// One collection item's two layers as the room's loader makes them on a worker (I3 step 10b, rv/app/collection_layers
// .hpp): the cover through the derived-texture cache (built, then a hit without reading it, then served without its
// source when the media root is out of reach or unknown), the generated label for a game without a cover and for a
// cover that is no image, the spine always; the layers valid for the collection's arrays in both formats; no layer
// generated without the fonts.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include <rv/app/collection_layers.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::app::CoverItem;
using rv::app::FrontSource;
using rv::app::ItemLayers;

namespace {

struct Fonts {
    rv::assets::FontFace title;
    rv::assets::FontFace detail;

    [[nodiscard]] rv::assets::LabelFonts labels() const noexcept { return {&title, &detail}; }
};

Fonts loadFonts() {
    const fs::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto title = rv::assets::FontFace::loadBundled(assets, rv::assets::kLabelTitleFontPath);
    auto detail = rv::assets::FontFace::loadBundled(assets, rv::assets::kLabelDetailFontPath);
    REQUIRE(title.has_value());
    REQUIRE(detail.has_value());
    return Fonts{std::move(*title), std::move(*detail)};
}

CoverItem itemOf(std::string coverPath) {
    CoverItem item;
    item.game = rv::catalog::GameId::fromValue(0x1234).value();
    item.title = "Amber Lantern";
    item.fallbackTitle = "Amber Lantern";
    item.systemName = "Lumen 16 Home System";
    item.year = "1993";
    item.seed = 42;
    item.coverPath = std::move(coverPath);
    item.frontKey = item.game.toString() + "/front";
    item.spineKey = item.game.toString() + "/spine";
    return item;
}

void checkLayers(const ItemLayers& layers, rv::gl::CollectionArrayFormat format) {
    REQUIRE(layers.front.has_value());
    REQUIRE(layers.spine.has_value());
    CHECK(rv::gl::validateLayerImage(*layers.front, rv::gl::CollectionClass::Front, format).has_value());
    CHECK(rv::gl::validateLayerImage(*layers.spine, rv::gl::CollectionClass::Spine, format).has_value());
    CHECK(layers.frontEntry.size() == 64);
    CHECK(layers.spineEntry.size() == 64);
    CHECK_FALSE(layers.error.has_value());
}

} // namespace

TEST_CASE("app/collection_layers: a cover through the cache, a hit, then served without its source",
          "[app][collection_layers]") {
    const rv::test::TempDir dir("collection-layers");
    const fs::path media = dir.file("media");
    const std::string coverPath = "snes/covers/Amber Lantern (USA).png";
    fs::create_directories(media / "snes" / "covers");
    REQUIRE(rv::assets::writePng(media / rv::core::pathFromUtf8(coverPath),
                                 rv::assets::makeCoverPlaceholder(680, 497, "Amber Lantern", 100))
                .has_value());
    const Fonts fonts = loadFonts();
    const CoverItem item = itemOf(coverPath);

    for (const rv::gl::CollectionArrayFormat format :
         {rv::gl::CollectionArrayFormat::Bc1Srgb, rv::gl::CollectionArrayFormat::Rgba8Srgb}) {
        INFO(rv::gl::toString(format));
        rv::assets::DerivedTextureCache cache(dir.file(std::string("cache-") + std::string(rv::gl::toString(format))));
        const ItemLayers built = rv::app::serveItemLayers(cache, fonts.labels(), format, media, item);
        checkLayers(built, format);
        CHECK(built.frontSource == FrontSource::Cover);
        CHECK(built.frontOutcome == rv::assets::ServeOutcome::Built);
        CHECK(built.spineOutcome == rv::assets::ServeOutcome::Built);
        CHECK(built.front->format == format);

        const ItemLayers hit = rv::app::serveItemLayers(cache, fonts.labels(), format, media, item);
        checkLayers(hit, format);
        CHECK(hit.frontOutcome == rv::assets::ServeOutcome::CacheHit);
        CHECK(hit.spineOutcome == rv::assets::ServeOutcome::CacheHit);
        CHECK(hit.front->levels == built.front->levels);
        CHECK(hit.frontEntry == built.frontEntry);

        // The media root unknown (no root to read under): the cover's entry found by its name.
        const ItemLayers unknown = rv::app::serveItemLayers(cache, fonts.labels(), format, std::nullopt, item);
        checkLayers(unknown, format);
        CHECK(unknown.frontSource == FrontSource::CachedCover);
        CHECK(unknown.front->levels == built.front->levels);
        // The spine keeps the cover's colour: the same entry.
        CHECK(unknown.spineEntry == built.spineEntry);

        // I3 step 12: the spine stands on the cover's dominant colour (ADR-0011 §5), not on the palette's colour of
        // the placement's seed that a game without a cover gets.
        const auto bytes = rv::core::readFileShared(media / rv::core::pathFromUtf8(coverPath), 1u << 26);
        REQUIRE(bytes.has_value());
        const auto cover = rv::assets::buildCoverLayer(std::as_bytes(std::span(bytes->data(), bytes->size())),
                                                       rv::app::textureFormatOf(format), "cover");
        REQUIRE(cover.has_value());
        rv::assets::SpineRequest spine;
        spine.title = item.title;
        spine.fallbackTitle = item.fallbackTitle;
        spine.coverColour = cover->texture.dominant;
        spine.seed = item.seed;
        CHECK(built.spineEntry ==
              rv::assets::spineCacheKey(spine, rv::app::textureFormatOf(format), fonts.labels()).hex());
        spine.coverColour.reset();
        CHECK(built.spineEntry !=
              rv::assets::spineCacheKey(spine, rv::app::textureFormatOf(format), fonts.labels()).hex());
    }

    // The cover gone, without an entry: the generated label instead.
    rv::assets::DerivedTextureCache empty(dir.file("cache-empty"));
    const ItemLayers gone = rv::app::serveItemLayers(empty, fonts.labels(), rv::gl::CollectionArrayFormat::Bc1Srgb,
                                                     dir.file("elsewhere"), item);
    checkLayers(gone, rv::gl::CollectionArrayFormat::Bc1Srgb);
    CHECK(gone.frontSource == FrontSource::LabelForCover);
    CHECK(gone.coverError.has_value());
}

TEST_CASE("app/collection_layers: generated labels for no cover and a cover that is no image; nothing without fonts",
          "[app][collection_layers]") {
    const rv::test::TempDir dir("collection-labels");
    const fs::path media = dir.file("media");
    const Fonts fonts = loadFonts();
    rv::assets::DerivedTextureCache cache(dir.file("cache"));
    constexpr auto kFormat = rv::gl::CollectionArrayFormat::Bc1Srgb;

    const ItemLayers label = rv::app::serveItemLayers(cache, fonts.labels(), kFormat, media, itemOf(""));
    checkLayers(label, kFormat);
    CHECK(label.frontSource == FrontSource::Label);
    CHECK_FALSE(label.coverError.has_value());

    const std::string text = "snes/covers/Parasol Patrol (USA).png";
    REQUIRE(rv::core::writeTextFileAtomic(media / rv::core::pathFromUtf8(text), "not an image\n").has_value());
    const ItemLayers broken = rv::app::serveItemLayers(cache, fonts.labels(), kFormat, media, itemOf(text));
    checkLayers(broken, kFormat);
    CHECK(broken.frontSource == FrontSource::LabelForCover);
    REQUIRE(broken.coverError.has_value());
    // The same title and seed: the same label as the one without a cover.
    CHECK(broken.front->levels == label.front->levels);

    const ItemLayers bare = rv::app::serveItemLayers(cache, rv::assets::LabelFonts{}, kFormat, media, itemOf(""));
    CHECK_FALSE(bare.front.has_value());
    CHECK_FALSE(bare.spine.has_value());
    REQUIRE(bare.error.has_value());
    CHECK(bare.error->code == rv::ErrorCode::Unavailable);
}

TEST_CASE("app/collection_layers: a family's label shape names and shapes its front (I4 step 2)",
          "[app][collection_layers]") {
    // ADR-0013 §4: the cover fitted to the room's family's label quad, its entry named with the shape; a game without
    // a cover gets a label laid out at that shape. The 16-bit family's items (the layer's shape) keep I3's entries.
    const rv::test::TempDir dir("collection-layers-shape");
    const fs::path media = dir.file("media");
    const std::string coverPath = "psx/covers/Amber Lantern (USA).png";
    fs::create_directories(media / "psx" / "covers");
    REQUIRE(rv::assets::writePng(media / rv::core::pathFromUtf8(coverPath),
                                 rv::assets::makeCoverPlaceholder(600, 600, "Amber Lantern", 100))
                .has_value());
    const Fonts fonts = loadFonts();
    rv::assets::DerivedTextureCache cache(dir.file("cache"));
    const auto format = rv::gl::CollectionArrayFormat::Bc1Srgb;
    const CoverItem layerItem = itemOf(coverPath);
    CoverItem squareItem = itemOf(coverPath);
    squareItem.label = {120, 120};
    const ItemLayers layer = rv::app::serveItemLayers(cache, fonts.labels(), format, media, layerItem);
    const ItemLayers square = rv::app::serveItemLayers(cache, fonts.labels(), format, media, squareItem);
    checkLayers(layer, format);
    checkLayers(square, format);
    CHECK(layer.frontEntry == rv::assets::coverCacheKey(rv::assets::TextureClass::Front,
                                                        rv::assets::TextureFormat::Bc1Srgb, "covers", coverPath)
                                  .hex());
    CHECK(square.frontEntry == rv::assets::coverCacheKey(rv::assets::TextureClass::Front,
                                                         rv::assets::TextureFormat::Bc1Srgb, "covers", coverPath,
                                                         squareItem.label)
                                   .hex());
    CHECK(square.frontEntry != layer.frontEntry);
    CHECK(square.front->levels != layer.front->levels);
    // The spine does not depend on the shape.
    CHECK(square.spineEntry == layer.spineEntry);

    CoverItem squareLabel = itemOf("");
    squareLabel.label = {120, 120};
    const ItemLayers generated = rv::app::serveItemLayers(cache, fonts.labels(), format, media, squareLabel);
    const ItemLayers i3Label = rv::app::serveItemLayers(cache, fonts.labels(), format, media, itemOf(""));
    checkLayers(generated, format);
    CHECK(generated.frontSource == FrontSource::Label);
    CHECK(generated.frontEntry != i3Label.frontEntry);
}
