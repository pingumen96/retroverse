#include <rv/assets/image_io.hpp>

#include <algorithm>
#include <array>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace rv::assets {

Expected<ImageData> loadImage(const std::filesystem::path& path, int desiredChannels) {
    auto bytes = core::readBinaryFile(path, kMaxImageFileBytes);
    if (!bytes) {
        return propagate(bytes.error());
    }
    return decodeImage(*bytes, desiredChannels, core::pathToUtf8(path));
}

Status writePng(const std::filesystem::path& path, const ImageData& image) {
    auto encoded = encodePng(image);
    if (!encoded) {
        Error error = encoded.error();
        error.context = core::pathToUtf8(path);
        return propagate(std::move(error));
    }
    return core::writeFileAtomic(path, *encoded);
}

void flipRowsInPlace(ImageData& image) {
    if (!image.isValid()) {
        return;
    }
    const std::size_t rowBytes = image.rowBytes();
    auto* pixels = image.pixels.data();
    for (std::size_t top = 0, bottom = static_cast<std::size_t>(image.height) - 1; top < bottom; ++top, --bottom) {
        std::swap_ranges(pixels + top * rowBytes, pixels + (top + 1) * rowBytes, pixels + bottom * rowBytes);
    }
}

ImageData convertChannels(const ImageData& image, int channels) {
    if (!image.isValid() || channels < 1 || channels > 4) {
        return {};
    }
    if (channels == image.channels) {
        return image;
    }
    ImageData result;
    result.width = image.width;
    result.height = image.height;
    result.channels = channels;
    const std::size_t pixelCount = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    result.pixels.resize(pixelCount * static_cast<std::size_t>(channels));

    const auto source = static_cast<std::size_t>(image.channels);
    const auto target = static_cast<std::size_t>(channels);
    const bool sourceHasColor = image.channels >= 3;
    const bool sourceHasAlpha = image.channels == 2 || image.channels == 4;
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::uint8_t* in = image.pixels.data() + i * source;
        std::array<std::uint8_t, 3> rgb{in[0], in[0], in[0]};
        if (sourceHasColor) {
            rgb = {in[0], in[1], in[2]};
        }
        const std::uint8_t alpha = sourceHasAlpha ? in[source - 1] : std::uint8_t{255};
        const auto grey =
            sourceHasColor ? static_cast<std::uint8_t>((rgb[0] * 77u + rgb[1] * 150u + rgb[2] * 29u) >> 8) : rgb[0];

        std::uint8_t* out = result.pixels.data() + i * target;
        switch (channels) {
        case 1:
            out[0] = grey;
            break;
        case 2:
            out[0] = grey;
            out[1] = alpha;
            break;
        case 3:
            out[0] = rgb[0];
            out[1] = rgb[1];
            out[2] = rgb[2];
            break;
        default:
            out[0] = rgb[0];
            out[1] = rgb[1];
            out[2] = rgb[2];
            out[3] = alpha;
            break;
        }
    }
    return result;
}

} // namespace rv::assets
