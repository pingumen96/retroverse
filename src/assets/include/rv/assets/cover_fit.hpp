#pragma once

// A cover fitted to its label without stretching (I3 step 5, ADR-0011 §5, style §12.2): scaled to cover the label and
// cropped (centred) when that crops at most 8% of the cropped side, else scaled to fit and the remaining bands filled
// with the mean colour of the image's edge facing them (the portrait covers get side bands); and the picture's
// dominant colour, which the spine's background starts from. Resampling uses stb_image_resize2 on sRGB data (filtered
// in linear light: Mitchell when reducing, Catmull-Rom when enlarging, edges clamped).

#include <cstddef>
#include <span>
#include <string_view>

#include <rv/assets/image_data.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/rvtex.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// The largest share of a side the cover may lose to the crop (ADR-0011 §5): 8% of the cropped side, half at each end.
inline constexpr double kMaxCoverCropShare = 0.08;

struct FitOptions {
    /// What transparent pixels are composited over, in linear light (covers are opaque; a cover with alpha shows this
    /// colour through): a dark neutral grey by default, the family's plastic colour when the caller has one.
    Rgb8 matte{0x3A, 0x3A, 0x3A};
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
/// Errors: InvalidArgument (an invalid image, a size below 1).
[[nodiscard]] Expected<FitResult> fitCover(const ImageData& source, int width, int height,
                                           const FitOptions& options = {});

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

/// The whole cold path of one cover on a worker: decode (PNG or JPEG, assets::decodeImage), fit to the front class's
/// size in `format`, the dominant colour, the mip chain in linear light and its encoding. Errors: those of
/// decodeImage() (Unsupported, ParseError for a truncated file, OutOfRange, InvalidArgument for no bytes), with
/// `context` (logged at debug level only by the callers: it names the user's file).
[[nodiscard]] Expected<CoverLayer> buildCoverLayer(std::span<const std::byte> bytes, TextureFormat format,
                                                   std::string_view context, const FitOptions& options = {});

} // namespace rv::assets
