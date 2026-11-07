// The only translation unit that compiles the stb_image_resize2 implementation (STB_IMAGE_RESIZE_STATIC: internal
// linkage). stb_image_resize2 keeps its state in the STBIR_RESIZE of each call, so concurrent fits are safe.

#include <rv/assets/cover_fit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <numeric>
#include <string>

#include <rv/assets/image_io.hpp>

#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

namespace rv::assets {
namespace {

/// dominantColour()'s colourful bins (I3 step 16): channel spread and brightest channel of the bin's centre, in sRGB
/// steps, and the share of the region they must hold together to be preferred over a neutral fullest bin.
constexpr int kMinDominantChroma = 48;
constexpr int kMinDominantBrightness = 64;
constexpr std::uint64_t kMinColourfulPermille = 150;

bool isOpaqueRgba(const ImageData& image) noexcept {
    if (image.channels != 4) {
        return false;
    }
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        if (image.pixels[i] != 255) {
            return false;
        }
    }
    return true;
}

/// The image as opaque RGBA8: grey expanded, alpha composited over the matte in linear light.
ImageData toOpaqueRgba(const ImageData& source, Rgb8 matte) {
    ImageData image = convertChannels(source, 4);
    const LinearRgb under = toLinear(matte);
    const std::size_t pixels = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint8_t* pixel = image.pixels.data() + i * 4;
        const std::uint8_t alphaCode = pixel[3];
        if (alphaCode == 255) {
            continue;
        }
        const float alpha = alphaCode / 255.0F;
        pixel[0] = linearToSrgbByte(srgbByteToLinear(pixel[0]) * alpha + under.r * (1.0F - alpha));
        pixel[1] = linearToSrgbByte(srgbByteToLinear(pixel[1]) * alpha + under.g * (1.0F - alpha));
        pixel[2] = linearToSrgbByte(srgbByteToLinear(pixel[2]) * alpha + under.b * (1.0F - alpha));
        pixel[3] = 255;
    }
    return image;
}

/// Resamples the region (s0, t0)-(s1, t1) of `input` (texture coordinates of the whole image) into `output`, whose size
/// is set; sRGB data filtered in linear light, stb_image_resize2's default filters (Mitchell reducing, Catmull-Rom
/// enlarging), edges clamped.
bool resizeSrgb(const ImageData& input, double s0, double t0, double s1, double t1, ImageData& output) {
    STBIR_RESIZE resize;
    stbir_resize_init(&resize, input.pixels.data(), input.width, input.height, input.width * 4, output.pixels.data(),
                      output.width, output.height, output.width * 4, STBIR_4CHANNEL, STBIR_TYPE_UINT8_SRGB);
    stbir_set_edgemodes(&resize, STBIR_EDGE_CLAMP, STBIR_EDGE_CLAMP);
    stbir_set_filters(&resize, STBIR_FILTER_DEFAULT, STBIR_FILTER_DEFAULT);
    if (stbir_set_input_subrect(&resize, s0, t0, s1, t1) == 0) {
        return false;
    }
    return stbir_resize_extended(&resize) != 0;
}

ImageData blankRgba(int width, int height) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, std::uint8_t{255});
    return image;
}

/// The mean, in linear light, of the strips along the two edges that face the bands: the left and right columns for
/// side bands, the top and bottom rows otherwise, each 2% of the side wide (at least one pixel).
Rgb8 edgeMean(const ImageData& rgba, bool sideBands) {
    const int across = sideBands ? rgba.width : rgba.height;
    const int strip = std::max(1, static_cast<int>(std::lround(across * 0.02)));
    double sum[3] = {0.0, 0.0, 0.0};
    std::size_t count = 0;
    const auto add = [&](int x, int y) {
        const std::uint8_t* pixel =
            rgba.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba.width) +
                                  static_cast<std::size_t>(x)) *
                                     4;
        for (std::size_t c = 0; c < 3; ++c) {
            sum[c] += srgbByteToLinear(pixel[c]);
        }
        ++count;
    };
    for (int y = 0; y < rgba.height; ++y) {
        for (int x = 0; x < rgba.width; ++x) {
            const int position = sideBands ? x : y;
            if (position < strip || position >= across - strip) {
                add(x, y);
            }
        }
    }
    if (count == 0) {
        return {};
    }
    return toSrgb8({static_cast<float>(sum[0] / static_cast<double>(count)),
                    static_cast<float>(sum[1] / static_cast<double>(count)),
                    static_cast<float>(sum[2] / static_cast<double>(count))});
}

} // namespace

std::string_view toString(FitMode mode) noexcept {
    return mode == FitMode::Cropped ? "cropped" : "banded";
}

LabelAspect LabelAspect::reduced() const noexcept {
    if (isLayer()) {
        return {};
    }
    const std::uint32_t divisor = std::gcd(width, height);
    return {width / divisor, height / divisor};
}

std::string LabelAspect::key() const {
    if (isLayer()) {
        return {};
    }
    const LabelAspect lowest = reduced();
    return std::to_string(lowest.width) + "x" + std::to_string(lowest.height);
}

CanvasSize labelCanvas(const LabelAspect& label, int layerWidth, int layerHeight) noexcept {
    if (label.isLayer() || layerWidth < 1 || layerHeight < 1) {
        return {layerWidth, layerHeight};
    }
    const double aspect = static_cast<double>(label.width) / static_cast<double>(label.height);
    const int width = std::max(layerWidth, static_cast<int>(std::lround(layerHeight * aspect)));
    const int height = std::max(layerHeight, static_cast<int>(std::lround(layerWidth / aspect)));
    return {width, height};
}

Expected<ImageData> resampleSrgb(const ImageData& rgba, int width, int height) {
    if (!rgba.isValid() || rgba.channels != 4 || width < 1 || height < 1) {
        return makeError(ErrorCode::InvalidArgument,
                         "cannot resample a " + std::to_string(rgba.width) + "x" + std::to_string(rgba.height) +
                             " picture to " + std::to_string(width) + "x" + std::to_string(height),
                         "picture");
    }
    try {
        ImageData output = blankRgba(width, height);
        if (!resizeSrgb(rgba, 0.0, 0.0, 1.0, 1.0, output)) {
            return makeError(ErrorCode::OutOfRange, "the picture could not be resampled", "picture");
        }
        return output;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while resampling a picture", "picture");
    }
}

Expected<ImageData> resampleSrgbAlpha(const ImageData& rgba, int width, int height) {
    if (!rgba.isValid() || rgba.channels != 4 || width < 1 || height < 1) {
        return makeError(ErrorCode::InvalidArgument,
                         "cannot resample a " + std::to_string(rgba.width) + "x" + std::to_string(rgba.height) +
                             " picture to " + std::to_string(width) + "x" + std::to_string(height),
                         "picture");
    }
    try {
        ImageData output = blankRgba(width, height);
        STBIR_RESIZE resize;
        // STBIR_RGBA: straight alpha, weighted by the resampler (premultiplied while filtering, divided again after).
        stbir_resize_init(&resize, rgba.pixels.data(), rgba.width, rgba.height, rgba.width * 4, output.pixels.data(),
                          output.width, output.height, output.width * 4, STBIR_RGBA, STBIR_TYPE_UINT8_SRGB);
        stbir_set_edgemodes(&resize, STBIR_EDGE_CLAMP, STBIR_EDGE_CLAMP);
        stbir_set_filters(&resize, STBIR_FILTER_DEFAULT, STBIR_FILTER_DEFAULT);
        if (stbir_resize_extended(&resize) == 0) {
            return makeError(ErrorCode::OutOfRange, "the picture could not be resampled", "picture");
        }
        return output;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while resampling a picture", "picture");
    }
}

Expected<FitResult> fitCover(const ImageData& source, int width, int height, const FitOptions& options) {
    if (!source.isValid() || width < 1 || height < 1) {
        return makeError(ErrorCode::InvalidArgument,
                         "cannot fit a " + std::to_string(source.width) + "x" + std::to_string(source.height) +
                             " image to " + std::to_string(width) + "x" + std::to_string(height),
                         "cover");
    }
    try {
        // An opaque RGBA image (what buildCoverLayer() decodes) is used as it is, anything else converted.
        ImageData converted;
        if (!isOpaqueRgba(source)) {
            converted = toOpaqueRgba(source, options.matte);
        }
        const ImageData& rgba = converted.isValid() ? converted : source;
        const double sourceWidth = rgba.width;
        const double sourceHeight = rgba.height;
        // The label the cover is fitted to, in its own units: the picture's pixels (I3's fit), or the label quad's
        // shape (ADR-0013 §4), whose units then map to the picture's pixels axis by axis (the one resample).
        const bool layer = options.label.isLayer();
        const LabelAspect shape = options.label.reduced();
        const double labelWidth = layer ? static_cast<double>(width) : static_cast<double>(shape.width);
        const double labelHeight = layer ? static_cast<double>(height) : static_cast<double>(shape.height);
        const double pixelsPerUnitX = layer ? 1.0 : width / labelWidth;
        const double pixelsPerUnitY = layer ? 1.0 : height / labelHeight;
        const double scaleX = labelWidth / sourceWidth;
        const double scaleY = labelHeight / sourceHeight;

        FitResult result;
        result.picture = blankRgba(width, height);
        result.bandColour = options.matte;

        // Scaled to cover: the axis with the smaller scale overflows and would be cropped.
        const double cover = std::max(scaleX, scaleY);
        const bool cropWidth = scaleY > scaleX; // scaled by the height, the width overflows
        const double kept = cropWidth ? labelWidth / (sourceWidth * cover) : labelHeight / (sourceHeight * cover);
        const double share = 1.0 - kept;
        if (share <= kMaxCoverCropShare + 1e-12) {
            result.mode = FitMode::Cropped;
            result.cropShare = share < 1e-12 ? 0.0 : share;
            const double margin = result.cropShare / 2.0;
            const double s0 = cropWidth ? margin : 0.0;
            const double t0 = cropWidth ? 0.0 : margin;
            if (!resizeSrgb(rgba, s0, t0, 1.0 - s0, 1.0 - t0, result.picture)) {
                return makeError(ErrorCode::OutOfRange, "the cover could not be resampled", "cover");
            }
            result.coverWidth = width;
            result.coverHeight = height;
            return result;
        }

        // Scaled to fit, bands of the edge's colour on the axis left uncovered.
        const double fit = std::min(scaleX, scaleY);
        const int coverWidth = std::clamp(static_cast<int>(std::lround(sourceWidth * fit * pixelsPerUnitX)), 1, width);
        const int coverHeight =
            std::clamp(static_cast<int>(std::lround(sourceHeight * fit * pixelsPerUnitY)), 1, height);
        const bool sideBands = coverWidth < width;
        ImageData scaled = blankRgba(coverWidth, coverHeight);
        if (!resizeSrgb(rgba, 0.0, 0.0, 1.0, 1.0, scaled)) {
            return makeError(ErrorCode::OutOfRange, "the cover could not be resampled", "cover");
        }
        result.mode = FitMode::Banded;
        result.bandColour = edgeMean(rgba, sideBands);
        result.coverX = (width - coverWidth) / 2;
        result.coverY = (height - coverHeight) / 2;
        result.coverWidth = coverWidth;
        result.coverHeight = coverHeight;
        const std::array<std::uint8_t, 4> band = {result.bandColour.r, result.bandColour.g, result.bandColour.b, 255};
        for (std::size_t i = 0; i < result.picture.pixels.size(); i += 4) {
            std::copy(band.begin(), band.end(), result.picture.pixels.begin() + static_cast<std::ptrdiff_t>(i));
        }
        const std::size_t rowBytes = static_cast<std::size_t>(coverWidth) * 4;
        for (int y = 0; y < coverHeight; ++y) {
            const std::uint8_t* in = scaled.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
            std::uint8_t* out = result.picture.pixels.data() +
                                (static_cast<std::size_t>(result.coverY + y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(result.coverX)) *
                                    4;
            std::copy(in, in + rowBytes, out);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while fitting the cover", "cover");
    }
}

Rgb8 dominantColour(const ImageData& image, int x, int y, int width, int height) {
    if (!image.isValid() || image.channels < 3) {
        return {};
    }
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > image.width || y + height > image.height) {
        x = 0;
        y = 0;
        width = image.width;
        height = image.height;
    }
    constexpr std::size_t kBins = 16 * 16 * 16;
    std::vector<std::uint32_t> counts(kBins, 0);
    const auto channels = static_cast<std::size_t>(image.channels);
    const auto binOf = [](const std::uint8_t* pixel) {
        return (static_cast<std::size_t>(pixel[0] >> 4) << 8) | (static_cast<std::size_t>(pixel[1] >> 4) << 4) |
               static_cast<std::size_t>(pixel[2] >> 4);
    };
    const auto pixelAt = [&](int px, int py) {
        return image.pixels.data() +
               (static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(px)) *
                   channels;
    };
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            ++counts[binOf(pixelAt(px, py))];
        }
    }
    // I3 step 16: a cover's fullest bin is often its frame (the black border of many 16-bit covers), which gave near
    // black spines; the fullest colourful bin is taken instead when the colourful bins hold enough of the picture.
    const auto isColourful = [](std::size_t bin) {
        const int r = static_cast<int>((bin >> 8) & 0xF) * 16 + 8;
        const int g = static_cast<int>((bin >> 4) & 0xF) * 16 + 8;
        const int b = static_cast<int>(bin & 0xF) * 16 + 8;
        const int high = std::max({r, g, b});
        const int low = std::min({r, g, b});
        return high - low >= kMinDominantChroma && high >= kMinDominantBrightness;
    };
    std::uint64_t colourfulPixels = 0;
    std::size_t fullestColourful = kBins;
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        if (counts[bin] == 0 || !isColourful(bin)) {
            continue;
        }
        colourfulPixels += counts[bin];
        if (fullestColourful == kBins || counts[bin] > counts[fullestColourful]) {
            fullestColourful = bin;
        }
    }
    const std::uint64_t regionPixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const auto fullest =
        fullestColourful != kBins && colourfulPixels * 1000 >= regionPixels * kMinColourfulPermille
            ? fullestColourful
            : static_cast<std::size_t>(std::distance(counts.begin(), std::max_element(counts.begin(), counts.end())));
    double sum[3] = {0.0, 0.0, 0.0};
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            const std::uint8_t* pixel = pixelAt(px, py);
            if (binOf(pixel) == fullest) {
                for (std::size_t c = 0; c < 3; ++c) {
                    sum[c] += srgbByteToLinear(pixel[c]);
                }
            }
        }
    }
    const double count = counts[fullest];
    return toSrgb8({static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count),
                    static_cast<float>(sum[2] / count)});
}

Expected<CoverLayer> buildCoverLayer(std::span<const std::byte> bytes, TextureFormat format, std::string_view context,
                                     const FitOptions& options) {
    auto decoded = decodeUprightImage(bytes, 4, context); // palettes, grey and 16-bit samples expanded by stb_image
    if (!decoded) {
        return propagate(decoded.error());
    }
    const LayerSize size = layerSize(TextureClass::Front, format);
    auto fitted = fitCover(*decoded, size.width, size.height, options);
    if (!fitted) {
        Error error = fitted.error();
        error.context = std::string(context);
        return propagate(std::move(error));
    }
    const Rgb8 dominant =
        dominantColour(fitted->picture, fitted->coverX, fitted->coverY, fitted->coverWidth, fitted->coverHeight);
    auto texture = makeLayerTexture(fitted->picture, TextureClass::Front, format, dominant);
    if (!texture) {
        return propagate(texture.error());
    }
    CoverLayer layer;
    layer.texture = std::move(*texture);
    layer.mode = fitted->mode;
    layer.cropShare = fitted->cropShare;
    layer.sourceWidth = decoded->width;
    layer.sourceHeight = decoded->height;
    return layer;
}

} // namespace rv::assets
