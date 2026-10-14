// The only translation unit that compiles the stb_image_write implementation. STB_IMAGE_WRITE_STATIC keeps its
// global settings (compression level, vertical flip on write) private to this file, so they keep their defaults.

#include <rv/assets/image_io.hpp>

#include <string>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace rv::assets {
namespace {

struct ByteSink {
    std::vector<std::byte> bytes;
    bool failed = false;
};

void appendToSink(void* context, void* data, int size) noexcept {
    auto* sink = static_cast<ByteSink*>(context);
    if (sink->failed || size <= 0) {
        return;
    }
    try {
        const auto* first = static_cast<const std::byte*>(data);
        sink->bytes.insert(sink->bytes.end(), first, first + size);
    } catch (...) {
        sink->failed = true; // never let an exception unwind through the C encoder
    }
}

Status checkEncodable(const ImageData& image, std::string_view format) {
    if (!image.isValid()) {
        return makeError(ErrorCode::InvalidArgument,
                         "cannot encode an invalid image (" + std::to_string(image.width) + "x" +
                             std::to_string(image.height) + ", " + std::to_string(image.channels) + " channels, " +
                             std::to_string(image.pixels.size()) + " bytes)",
                         std::string(format));
    }
    if (image.width > kMaxImageDimension || image.height > kMaxImageDimension) {
        return makeError(ErrorCode::OutOfRange,
                         "image sides are limited to " + std::to_string(kMaxImageDimension) + " pixels",
                         std::string(format));
    }
    return ok();
}

} // namespace

Expected<std::vector<std::byte>> encodePng(const ImageData& image) {
    if (auto valid = checkEncodable(image, "PNG"); !valid) {
        return propagate(valid.error());
    }
    ByteSink sink;
    const int stride = image.width * image.channels;
    const int written = stbi_write_png_to_func(&appendToSink, &sink, image.width, image.height, image.channels,
                                               image.pixels.data(), stride);
    if (written == 0 || sink.failed) {
        return makeError(ErrorCode::IoError, "PNG encoding failed", "PNG");
    }
    return std::move(sink.bytes);
}

Expected<std::vector<std::byte>> encodeJpeg(const ImageData& image, int quality) {
    if (auto valid = checkEncodable(image, "JPEG"); !valid) {
        return propagate(valid.error());
    }
    if (quality < 1 || quality > 100) {
        return makeError(ErrorCode::InvalidArgument, "JPEG quality must be in [1, 100], got " + std::to_string(quality),
                         "JPEG");
    }
    ByteSink sink;
    const int written = stbi_write_jpg_to_func(&appendToSink, &sink, image.width, image.height, image.channels,
                                               image.pixels.data(), quality);
    if (written == 0 || sink.failed) {
        return makeError(ErrorCode::IoError, "JPEG encoding failed", "JPEG");
    }
    return std::move(sink.bytes);
}

} // namespace rv::assets
