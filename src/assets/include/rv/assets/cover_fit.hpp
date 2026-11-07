#pragma once

// A cover fitted to its label without stretching (I3 step 5, ADR-0011 §5, style §12.2): scaled to cover the label and
// cropped (centred) when that crops at most 8% of the cropped side, else scaled to fit and the remaining bands filled
// with the mean colour of the image's edge facing them (the portrait covers get side bands); and the picture's
// dominant colour, which the spine's background starts from. Resampling uses stb_image_resize2 on sRGB data (filtered
// in linear light: Mitchell when reducing, Catmull-Rom when enlarging, edges clamped).
//
// The label's shape (I4 step 2, ADR-0013 §4): the front layer is always 256 x 188, and its item's label quad shows it
// stretched to the quad's own shape (a jewel case's square insert, a keep case's portrait one). A cover is therefore
// fitted to the label quad's aspect (LabelAspect: the crop or the bands decided in the source's pixels against that
// shape) and resampled once into the layer, so that the quad undoes the stretch; the generated front label is laid out
// at that aspect and resampled likewise. The default shape is the layer's own: I3's fit, which the 16-bit cartridges
// keep (ADR-0013 §4's stated exception: no pixel of theirs moves).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <rv/assets/image_data.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/rvtex.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// The largest share of a side the cover may lose to the crop (ADR-0011 §5): 8% of the cropped side, half at each end.
inline constexpr double kMaxCoverCropShare = 0.08;

/// The shape a front picture is made for (ADR-0013 §4): its family's label quad, width by height in one unit (the
/// family's millimetres). 0 x 0 is the front layer's own shape: I3's fit, the 16-bit cartridges'.
struct LabelAspect {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /// The layer's own shape (I3's fit).
    [[nodiscard]] bool isLayer() const noexcept { return width == 0 || height == 0; }
    /// The shape in lowest terms ({120, 120} is {1, 1}); the layer's is {0, 0}.
    [[nodiscard]] LabelAspect reduced() const noexcept;
    /// "<width>x<height>" in lowest terms, "" for the layer's: the part of a front layer's cache key that names the
    /// shape (derived_texture_cache.hpp), so that two families of one shape share their entries.
    [[nodiscard]] std::string key() const;

    bool operator==(const LabelAspect&) const = default;
};

/// The size of the canvas a front picture of `label`'s shape is made on before its one resample into a front layer of
/// `layerWidth` x `layerHeight`: the label's aspect, at least the layer's size on both sides, so that neither side of
/// the layer gets fewer pixels than it holds (a portrait label 256 x 360 for BC1's 256 x 188, a landscape one
/// 361 x 188). The layer's own size for the layer's shape.
struct CanvasSize {
    int width = 0;
    int height = 0;

    bool operator==(const CanvasSize&) const = default;
};
[[nodiscard]] CanvasSize labelCanvas(const LabelAspect& label, int layerWidth, int layerHeight) noexcept;

struct FitOptions {
    /// What transparent pixels are composited over, in linear light (covers are opaque; a cover with alpha shows this
    /// colour through): a dark neutral grey by default, the family's plastic colour when the caller has one.
    Rgb8 matte{0x3A, 0x3A, 0x3A};
    /// The shape the cover is fitted to (the item's label quad), the picture's own size by default (I3's fit).
    LabelAspect label;
};

enum class FitMode {
    Cropped, ///< scaled to cover the label, the overflow cropped (at most kMaxCoverCropShare of the cropped side)
    Banded,  ///< scaled to fit inside the label, bands of the edge's mean colour beside it (or above and below)
};

[[nodiscard]] std::string_view toString(FitMode mode) noexcept; ///< "cropped", "banded"

struct FitResult {
    ImageData picture; ///< RGBA8 sRGB, opaque, exactly the size asked for
    FitMode mode = FitMode::Cropped;
    /// The share of the source's cropped side cut away (0 when nothing is cropped or the picture is banded).
    double cropShare = 0.0;
    /// Where the scaled cover lies in the picture (the whole picture when cropped).
    int coverX = 0;
    int coverY = 0;
    int coverWidth = 0;
    int coverHeight = 0;
    /// The bands' colour (the mean of the edges facing them; the matte when not banded).
    Rgb8 bandColour;
};

/// Fits a decoded image (1 to 4 channels: grey is expanded, alpha composited over FitOptions::matte) to width x height.
/// With FitOptions::label set, the crop or the bands are decided against the label's shape and the result resampled
/// into width x height at once (the cover's place, coverX to coverHeight, in the picture's pixels). Errors:
/// InvalidArgument (an invalid image, a size below 1).
[[nodiscard]] Expected<FitResult> fitCover(const ImageData& source, int width, int height,
                                           const FitOptions& options = {});

/// An opaque RGBA8 sRGB picture resampled to width x height (stb_image_resize2 in linear light, the filters above),
/// both sides independently: a picture made at its label's aspect squeezed into the front layer. Errors:
/// InvalidArgument (not RGBA, a size below 1), OutOfRange (the resampler failed).
[[nodiscard]] Expected<ImageData> resampleSrgb(const ImageData& rgba, int width, int height);

/// An RGBA8 sRGB picture with straight alpha (a logo, I11 step 2) resampled to width x height in linear light, its
/// colours weighted by their alpha, so that transparent pixels never bleed their colour into the edges. Errors as
/// resampleSrgb().
[[nodiscard]] Expected<ImageData> resampleSrgbAlpha(const ImageData& rgba, int width, int height);

/// The most common colour of the region (x, y, w, h) of an RGBA8 or RGB image: the pixels counted in bins of 4 bits per
/// channel, the fullest bin chosen (the lowest on a tie) and the mean of its pixels taken in linear light. Colourful
/// bins come first (I3 step 16: a cover's black or white frame made monotone spines): when the bins whose centre has a
/// channel spread of at least 48 and a brightest channel of at least 64 (sRGB steps) hold 15% of the region together,
/// the fullest of them is chosen (the lowest on a tie). The whole image when the region is empty or outside it; black
/// for an invalid image.
[[nodiscard]] Rgb8 dominantColour(const ImageData& image, int x = 0, int y = 0, int width = 0, int height = 0);

/// A cover's front layer and what the fit decided.
struct CoverLayer {
    RvTexture texture; ///< front class, `format`, dominant colour set, no source stamp (the caller sets it)
    FitMode mode = FitMode::Cropped;
    double cropShare = 0.0;
    int sourceWidth = 0;
    int sourceHeight = 0;
};

/// The whole cold path of one cover on a worker: decode (PNG or JPEG, assets::decodeUprightImage: its EXIF orientation
/// applied, the fix of 2026-10-29), fit to the front class's size in `format` (to FitOptions::label's shape, resampled
/// into the layer), the dominant colour, the mip chain in linear light and its encoding. Errors: those of
/// decodeImage() (Unsupported, ParseError for a truncated file, OutOfRange, InvalidArgument for no bytes), with
/// `context` (logged at debug level only by the callers: it names the user's file).
[[nodiscard]] Expected<CoverLayer> buildCoverLayer(std::span<const std::byte> bytes, TextureFormat format,
                                                   std::string_view context, const FitOptions& options = {});

} // namespace rv::assets
