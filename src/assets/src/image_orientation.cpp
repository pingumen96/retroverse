// The EXIF orientation of the user's pictures (the fix of 2026-10-29): stb_image decodes the stored rows and ignores
// the orientation tag, so a cover a phone or an editor saved turned (its pixels stored sideways, the tag saying how to
// stand them up) drew sideways on its label while image viewers and ES-DE show it upright. exifOrientation() reads the
// tag from a JPEG's APP1 "Exif" segment or a PNG's eXIf chunk (bounded, never failing), orientImage() applies it.

#include <rv/assets/image_io.hpp>

#include <cstdint>
#include <optional>

namespace rv::assets {
namespace {

[[nodiscard]] std::uint8_t byteAt(std::span<const std::byte> bytes, std::size_t i) noexcept {
    return static_cast<std::uint8_t>(bytes[i]);
}

[[nodiscard]] std::uint32_t bigEndian32(std::span<const std::byte> bytes, std::size_t i) noexcept {
    return (static_cast<std::uint32_t>(byteAt(bytes, i)) << 24) |
           (static_cast<std::uint32_t>(byteAt(bytes, i + 1)) << 16) |
           (static_cast<std::uint32_t>(byteAt(bytes, i + 2)) << 8) | byteAt(bytes, i + 3);
}

/// The orientation tag of a TIFF block (an EXIF payload after its "Exif\0\0" header in JPEG, the whole eXIf chunk in
/// PNG): IFD0's tag 0x0112, a SHORT (or, leniently, a LONG) of 1 to 8. Nothing when absent or unreadable.
[[nodiscard]] std::optional<int> tiffOrientation(std::span<const std::byte> tiff) noexcept {
    if (tiff.size() < 8) {
        return std::nullopt;
    }
    bool little = false;
    if (byteAt(tiff, 0) == 'I' && byteAt(tiff, 1) == 'I') {
        little = true;
    } else if (!(byteAt(tiff, 0) == 'M' && byteAt(tiff, 1) == 'M')) {
        return std::nullopt;
    }
    const auto u16 = [&](std::size_t i) -> std::uint32_t {
        const std::uint32_t a = byteAt(tiff, i);
        const std::uint32_t b = byteAt(tiff, i + 1);
        return little ? (a | (b << 8)) : ((a << 8) | b);
    };
    const auto u32 = [&](std::size_t i) -> std::uint32_t {
        return little ? (u16(i) | (u16(i + 2) << 16)) : ((u16(i) << 16) | u16(i + 2));
    };
    if (u16(2) != 42) {
        return std::nullopt;
    }
    const std::size_t ifd = u32(4);
    if (ifd < 8 || ifd > tiff.size() || tiff.size() - ifd < 2) {
        return std::nullopt;
    }
    const std::size_t count = u16(ifd);
    for (std::size_t entry = 0; entry < count; ++entry) {
        const std::size_t at = ifd + 2 + entry * 12;
        if (at > tiff.size() || tiff.size() - at < 12) {
            return std::nullopt;
        }
        if (u16(at) != 0x0112) {
            continue;
        }
        const std::uint32_t type = u16(at + 2);
        std::uint32_t value = 0;
        if (type == 3) { // SHORT, left-justified in the value field
            value = u16(at + 8);
        } else if (type == 4) { // LONG
            value = u32(at + 8);
        } else {
            return std::nullopt;
        }
        if (value >= 1 && value <= 8) {
            return static_cast<int>(value);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> jpegOrientation(std::span<const std::byte> bytes) noexcept {
    std::size_t pos = 2; // after SOI
    while (pos + 3 < bytes.size()) {
        if (byteAt(bytes, pos) != 0xFF) {
            ++pos; // padding between segments
            continue;
        }
        const std::uint8_t marker = byteAt(bytes, pos + 1);
        if (marker == 0xFF) {
            ++pos; // fill byte
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { // no length field
            pos += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) { // the first scan, or the end: no tag before it
            return std::nullopt;
        }
        const std::size_t length = (static_cast<std::size_t>(byteAt(bytes, pos + 2)) << 8) | byteAt(bytes, pos + 3);
        if (length < 2 || pos + 2 + length > bytes.size()) {
            return std::nullopt;
        }
        if (marker == 0xE1 && length >= 2 + 6) {
            const std::span<const std::byte> payload = bytes.subspan(pos + 4, length - 2);
            static constexpr std::uint8_t kExif[6] = {'E', 'x', 'i', 'f', 0, 0};
            bool exif = true;
            for (std::size_t i = 0; i < 6; ++i) {
                exif = exif && byteAt(payload, i) == kExif[i];
            }
            if (exif) {
                // The first EXIF segment is the one (a second APP1 is XMP or an extension).
                return tiffOrientation(payload.subspan(6));
            }
        }
        pos += 2 + length;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> pngOrientation(std::span<const std::byte> bytes) noexcept {
    std::size_t pos = 8; // after the signature
    while (pos + 12 <= bytes.size()) {
        const std::size_t length = bigEndian32(bytes, pos);
        const std::uint32_t type = bigEndian32(bytes, pos + 4);
        if (length > bytes.size() - pos - 12) {
            return std::nullopt;
        }
        constexpr std::uint32_t kExif = 0x65584966u; // "eXIf"
        constexpr std::uint32_t kData = 0x49444154u; // "IDAT": eXIf comes before it (PNG 1.5's rule)
        constexpr std::uint32_t kEnd = 0x49454E44u;  // "IEND"
        if (type == kExif) {
            return tiffOrientation(bytes.subspan(pos + 8, length));
        }
        if (type == kData || type == kEnd) {
            return std::nullopt;
        }
        pos += 12 + length;
    }
    return std::nullopt;
}

} // namespace

int exifOrientation(std::span<const std::byte> bytes) noexcept {
    std::optional<int> orientation;
    if (bytes.size() >= 8 && byteAt(bytes, 0) == 0x89 && byteAt(bytes, 1) == 'P' && byteAt(bytes, 2) == 'N' &&
        byteAt(bytes, 3) == 'G') {
        orientation = pngOrientation(bytes);
    } else if (bytes.size() >= 4 && byteAt(bytes, 0) == 0xFF && byteAt(bytes, 1) == 0xD8) {
        orientation = jpegOrientation(bytes);
    }
    return orientation.value_or(1);
}

ImageData orientImage(const ImageData& image, int orientation) {
    if (!image.isValid() || orientation <= 1 || orientation > 8) {
        return image;
    }
    const int w = image.width;
    const int h = image.height;
    const bool swaps = orientation >= 5;
    ImageData result;
    result.width = swaps ? h : w;
    result.height = swaps ? w : h;
    result.channels = image.channels;
    result.pixels.resize(image.pixels.size());
    const auto channels = static_cast<std::size_t>(image.channels);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            // The stored pixel that stands at (x, y) once upright.
            int sx = x;
            int sy = y;
            switch (orientation) {
            case 2:
                sx = w - 1 - x;
                break;
            case 3:
                sx = w - 1 - x;
                sy = h - 1 - y;
                break;
            case 4:
                sy = h - 1 - y;
                break;
            case 5:
                sx = y;
                sy = x;
                break;
            case 6:
                sx = y;
                sy = h - 1 - x;
                break;
            case 7:
                sx = w - 1 - y;
                sy = h - 1 - x;
                break;
            default: // 8
                sx = w - 1 - y;
                sy = x;
                break;
            }
            const std::size_t from =
                (static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)) * channels;
            const std::size_t to =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(result.width) + static_cast<std::size_t>(x)) *
                channels;
            for (std::size_t c = 0; c < channels; ++c) {
                result.pixels[to + c] = image.pixels[from + c];
            }
        }
    }
    return result;
}

Expected<ImageData> decodeUprightImage(std::span<const std::byte> bytes, int desiredChannels,
                                       std::string_view context) {
    auto image = decodeImage(bytes, desiredChannels, context);
    if (!image) {
        return image;
    }
    const int orientation = exifOrientation(bytes);
    if (orientation == 1) {
        return image;
    }
    return orientImage(*image, orientation);
}

} // namespace rv::assets
