// The only translation unit that compiles the stb_image implementation. STB_IMAGE_STATIC gives every stb
// function and its global state (including the vertical-flip flag) internal linkage, so no other code can change
// how Retroverse decodes images. Dimensions are checked against kMaxImageDimension after the header probe
// (stbi_info_from_memory) and before decoding, so oversized images fail with a clear OutOfRange error instead of
// stb's generic "too large".
//
// Thread safety: stb_image keeps its state per call (failure reason and flip override are thread-local). The one
// exception is the message for an unknown critical PNG chunk, which stb formats into a static buffer: concurrent
// decoding of such corrupt files can race on that error text only, never on pixel data.

#include <rv/assets/image_io.hpp>

#include <climits>
#include <cstdint>
#include <memory>
#include <new>
#include <string>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>

namespace rv::assets {
namespace {

enum class ImageFormat { Unknown, Png, Jpeg };

ImageFormat detectFormat(std::span<const std::byte> bytes) noexcept {
    static constexpr unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size() >= sizeof(kPngSignature)) {
        bool png = true;
        for (std::size_t i = 0; i < sizeof(kPngSignature); ++i) {
            png = png && static_cast<unsigned char>(bytes[i]) == kPngSignature[i];
        }
        if (png) {
            return ImageFormat::Png;
        }
    }
    if (bytes.size() >= 3 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xD8} &&
        bytes[2] == std::byte{0xFF}) {
        return ImageFormat::Jpeg;
    }
    return ImageFormat::Unknown;
}

/// stb_image decodes truncated JPEG entropy data as zeros instead of failing. Walk the marker segments up to the
/// first start-of-scan (skipping APPn payloads such as EXIF thumbnails, which carry their own markers) and require
/// an end-of-image marker after it.
bool jpegHasEndOfImage(std::span<const std::byte> bytes) noexcept {
    const auto at = [&bytes](std::size_t i) {
        return static_cast<unsigned char>(bytes[i]);
    };
    std::size_t pos = 2; // after SOI
    while (pos + 1 < bytes.size()) {
        if (at(pos) != 0xFF) {
            return false;
        }
        unsigned char marker = at(pos + 1);
        while (marker == 0xFF && pos + 2 < bytes.size()) { // fill bytes
            ++pos;
            marker = at(pos + 1);
        }
        if (marker == 0xD9) {
            return true;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { // markers without a length field
            pos += 2;
            continue;
        }
        if (pos + 3 >= bytes.size()) {
            return false;
        }
        const std::size_t length = (static_cast<std::size_t>(at(pos + 2)) << 8) | at(pos + 3);
        if (length < 2) {
            return false;
        }
        if (marker == 0xDA) { // start of scan: entropy-coded data follows the header
            for (std::size_t i = pos + 2 + length; i + 1 < bytes.size(); ++i) {
                if (at(i) == 0xFF && at(i + 1) == 0xD9) {
                    return true;
                }
            }
            return false;
        }
        pos += 2 + length;
    }
    return false;
}

std::string failureReason() {
    const char* reason = stbi_failure_reason();
    return reason != nullptr ? std::string(reason) : std::string("unknown error");
}

struct StbFree {
    void operator()(stbi_uc* pixels) const noexcept { stbi_image_free(pixels); }
};

} // namespace

Expected<ImageData> decodeImage(std::span<const std::byte> bytes, int desiredChannels, std::string_view context) {
    const std::string where(context);
    if (desiredChannels < 0 || desiredChannels > 4) {
        return makeError(ErrorCode::InvalidArgument,
                         "desiredChannels must be 0 (keep) or 1..4, got " + std::to_string(desiredChannels), where);
    }
    if (bytes.empty()) {
        return makeError(ErrorCode::InvalidArgument, "image data is empty", where);
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::OutOfRange, "image data is larger than 2 GiB", where);
    }
    const ImageFormat format = detectFormat(bytes);
    if (format == ImageFormat::Unknown) {
        return makeError(ErrorCode::Unsupported, "unrecognized image format (only PNG and JPEG are supported)", where);
    }
    const std::string formatName = format == ImageFormat::Png ? "PNG" : "JPEG";

    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int length = static_cast<int>(bytes.size());
    int width = 0;
    int height = 0;
    int fileChannels = 0;
    if (stbi_info_from_memory(data, length, &width, &height, &fileChannels) == 0) {
        return makeError(ErrorCode::ParseError, "corrupt " + formatName + " header (" + failureReason() + ")", where);
    }
    if (width <= 0 || height <= 0 || width > kMaxImageDimension || height > kMaxImageDimension) {
        return makeError(ErrorCode::OutOfRange,
                         formatName + " image is " + std::to_string(width) + "x" + std::to_string(height) +
                             " pixels; the limit is " + std::to_string(kMaxImageDimension) + " per side",
                         where);
    }
    if (format == ImageFormat::Jpeg && !jpegHasEndOfImage(bytes)) {
        return makeError(ErrorCode::ParseError, "truncated JPEG data (no end-of-image marker)", where);
    }

    try {
        // Explicitly per thread: the global flag cannot be reached from outside this file anyway.
        stbi_set_flip_vertically_on_load_thread(0);
        int decodedChannels = 0;
        std::unique_ptr<stbi_uc, StbFree> pixels(
            stbi_load_from_memory(data, length, &width, &height, &decodedChannels, desiredChannels));
        if (!pixels) {
            return makeError(ErrorCode::ParseError,
                             "corrupt or truncated " + formatName + " data (" + failureReason() + ")", where);
        }
        ImageData image;
        image.width = width;
        image.height = height;
        image.channels = desiredChannels == 0 ? decodedChannels : desiredChannels;
        const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                 static_cast<std::size_t>(image.channels);
        image.pixels.assign(pixels.get(), pixels.get() + size);
        return image;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while decoding the image", where);
    }
}

} // namespace rv::assets
