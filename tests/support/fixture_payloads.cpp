#include "support/fixture_payloads.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::test {
namespace {

constexpr int kMaxPictureSide = 4096;

tl::unexpected<Error> invalid(std::string_view context, std::string message) {
    return makeError(ErrorCode::ValidationFailed, fmt::format("{}: {}", context, message));
}

std::string lowerExtension(std::string_view extension) {
    return core::toLowerAscii(extension);
}

/// Every key of `value` one of `keys`.
Status onlyKeys(const nlohmann::json& value, std::initializer_list<std::string_view> keys, std::string_view what,
                std::string_view context) {
    if (!value.is_object()) {
        return invalid(context, fmt::format("\"{}\" must be an object", what));
    }
    for (const auto& [key, item] : value.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            std::string names;
            for (const std::string_view name : keys) {
                names += names.empty() ? "" : ", ";
                names += name;
            }
            return invalid(context, fmt::format("unknown key \"{}\" of \"{}\" ({})", key, what, names));
        }
    }
    return ok();
}

Expected<int> intIn(const nlohmann::json& value, std::string_view key, int low, int high, int fallback,
                    std::string_view context) {
    if (!value.contains(key)) {
        return fallback;
    }
    const nlohmann::json& item = value.at(std::string(key));
    if (!item.is_number_integer() || item.get<std::int64_t>() < low || item.get<std::int64_t>() > high) {
        return invalid(context, fmt::format("\"{}\" must be an integer in [{}, {}]", key, low, high));
    }
    return item.get<int>();
}

Expected<std::uint32_t> requiredSeed(const nlohmann::json& value, std::string_view context) {
    if (!value.contains("seed")) {
        return invalid(context, "\"seed\" is required");
    }
    const nlohmann::json& seed = value.at("seed");
    if (!seed.is_number_unsigned() || seed.get<std::uint64_t>() > 0xFFFFFFFFull) {
        return invalid(context, "\"seed\" must be an integer in [0, 4294967295]");
    }
    return seed.get<std::uint32_t>();
}

Expected<std::string> optionalWord(const nlohmann::json& value, std::string_view key, std::string_view context) {
    if (!value.contains(key)) {
        return std::string();
    }
    const nlohmann::json& item = value.at(std::string(key));
    if (!item.is_string() || item.get<std::string>().empty()) {
        return invalid(context, fmt::format("\"{}\" must be a non-empty string", key));
    }
    return item.get<std::string>();
}

bool insideRoundedRect(int x, int y, int left, int top, int right, int bottom, int radius) {
    if (x < left || x >= right || y < top || y >= bottom) {
        return false;
    }
    const int cx = std::clamp(x, left + radius, right - 1 - radius);
    const int cy = std::clamp(y, top + radius, bottom - 1 - radius);
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

void put(assets::ImageData& image, int x, int y, std::array<std::uint8_t, 4> color) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4u;
    std::copy(color.begin(), color.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(at));
}

/// A cartridge seen from the front: a grey shell with rounded corners, a darker grip band along the top, ridges below
/// the label, the cover as the label; transparent around it.
assets::ImageData cartridgeImage(const PicturePayload& payload) {
    const int width = payload.width;
    const int height = payload.height;
    assets::ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);

    // The shell's grey varies a little with the seed: cartridges of one system are never all alike.
    const auto shade = static_cast<std::uint8_t>(150 + (payload.seed % 5u) * 6u);
    const std::array<std::uint8_t, 4> shell{shade, shade, static_cast<std::uint8_t>(shade + 4), 255};
    const std::array<std::uint8_t, 4> grip{static_cast<std::uint8_t>(shade - 40), static_cast<std::uint8_t>(shade - 40),
                                           static_cast<std::uint8_t>(shade - 36), 255};
    const int margin = std::max(1, std::min(width, height) / 25);
    const int radius = std::max(0, std::min(width, height) / 14);
    const int left = margin;
    const int top = margin;
    const int right = width - margin;
    const int bottom = height - margin;
    const int gripBottom = top + (bottom - top) * 12 / 100;
    const int ridgesTop = top + (bottom - top) * 78 / 100;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!insideRoundedRect(x, y, left, top, right, bottom, radius)) {
                continue;
            }
            const bool ridge = y >= ridgesTop && ((y - ridgesTop) / std::max(1, height / 60)) % 2 == 1;
            put(image, x, y, y < gripBottom || ridge ? grip : shell);
        }
    }

    // The label: the cover's pattern and title, inset in the shell's face.
    const int labelLeft = left + (right - left) * 12 / 100;
    const int labelRight = right - (right - left) * 12 / 100;
    const int labelTop = gripBottom + (bottom - top) * 5 / 100;
    const int labelBottom = ridgesTop - (bottom - top) * 4 / 100;
    if (labelRight - labelLeft >= 1 && labelBottom - labelTop >= 1) {
        const assets::ImageData label =
            assets::makeCoverPlaceholder(labelRight - labelLeft, labelBottom - labelTop, payload.title, payload.seed);
        for (int y = 0; y < label.height; ++y) {
            for (int x = 0; x < label.width; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(label.width) +
                                        static_cast<std::size_t>(x)) *
                                       4u;
                put(image, labelLeft + x, labelTop + y,
                    {label.pixels[at], label.pixels[at + 1], label.pixels[at + 2], 255});
            }
        }
    }
    return image;
}

std::string toBytes(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/// Bits written least significant first, as deflate packs them, into a buffer the caller sized for them (raw pointers:
/// Debug builds check every std::string access, and the writer runs once per byte of the image).
class BitWriter {
public:
    explicit BitWriter(char* out) : m_start(out), m_cursor(out) {}
    void bits(std::uint32_t value, int count) {
        m_buffer |= static_cast<std::uint64_t>(value) << m_count;
        m_count += count;
        while (m_count >= 8) {
            *m_cursor++ = static_cast<char>(m_buffer & 0xFFu);
            m_buffer >>= 8;
            m_count -= 8;
        }
    }
    /// Pads the last byte; returns the bytes written.
    std::size_t finish() {
        if (m_count > 0) {
            bits(0, 8 - m_count);
        }
        return static_cast<std::size_t>(m_cursor - m_start);
    }

private:
    char* m_start;
    char* m_cursor;
    std::uint64_t m_buffer = 0;
    int m_count = 0;
};

/// A literal or a length symbol (0..287) with deflate's fixed Huffman code (RFC 1951 §3.2.6).
void fixedSymbol(BitWriter& writer, std::uint32_t symbol) {
    struct Code {
        std::uint32_t bits = 0; // reversed: ready for BitWriter::bits()
        int length = 0;
    };
    static const std::array<Code, 288> kCodes = [] {
        std::array<Code, 288> codes{};
        for (std::uint32_t s = 0; s < 288; ++s) {
            std::uint32_t value = 0;
            int length = 0;
            if (s < 144) {
                value = 0x30 + s;
                length = 8;
            } else if (s < 256) {
                value = 0x190 + (s - 144);
                length = 9;
            } else if (s < 280) {
                value = s - 256;
                length = 7;
            } else {
                value = 0xC0 + (s - 280);
                length = 8;
            }
            std::uint32_t reversed = 0;
            for (int i = 0; i < length; ++i) {
                reversed |= ((value >> i) & 1u) << (length - 1 - i);
            }
            codes[s] = Code{reversed, length};
        }
        return codes;
    }();
    writer.bits(kCodes[symbol].bits, kCodes[symbol].length);
}

/// A match of `length` (3..258) bytes at distance 1: the previous byte repeated.
void repeatPrevious(BitWriter& writer, std::uint32_t length) {
    static constexpr std::array<std::uint16_t, 29> kBase{3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                         15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<std::uint8_t, 29> kExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    std::size_t index = kBase.size() - 1;
    while (kBase[index] > length) {
        --index;
    }
    fixedSymbol(writer, static_cast<std::uint32_t>(257 + index));
    writer.bits(length - kBase[index], kExtra[index]);
    writer.bits(0, 5); // distance code 0 (five zero bits whichever way they are read): distance 1
}

std::uint32_t crc32(std::string_view bytes) {
    static const std::array<std::uint32_t, 256> kTable = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        return table;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : bytes) {
        crc = kTable[(crc ^ static_cast<std::uint8_t>(c)) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void bigEndian(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out += static_cast<char>((value >> shift) & 0xFFu);
    }
}

void chunk(std::string& out, std::string_view type, std::string_view data) {
    bigEndian(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out += type;
    out += data;
    bigEndian(out, crc32(std::string_view(out).substr(start)));
}

} // namespace

std::string encodeQuickPng(const assets::ImageData& image) {
    if (!image.isValid()) {
        return {};
    }
    const int bpp = image.channels;
    const std::size_t rowBytes = image.rowBytes();
    // Filtered scanlines: per row "Sub" or "Up", whichever leaves more zero bytes (flat runs and repeated rows).
    const auto step = static_cast<std::size_t>(bpp);
    std::string filtered((rowBytes + 1) * static_cast<std::size_t>(image.height), '\0');
    for (int y = 0; y < image.height; ++y) {
        const std::uint8_t* row = image.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        char* out = filtered.data() + static_cast<std::size_t>(y) * (rowBytes + 1);
        std::size_t subZeros = 0;
        std::size_t upZeros = 0;
        for (std::size_t i = 0; i < rowBytes; ++i) {
            const std::uint8_t left = i >= step ? row[i - step] : std::uint8_t{0};
            const std::uint8_t above = y > 0 ? row[i - rowBytes] : std::uint8_t{0};
            subZeros += row[i] == left ? 1 : 0;
            upZeros += row[i] == above ? 1 : 0;
        }
        const bool useUp = upZeros > subZeros;
        out[0] = static_cast<char>(useUp ? 2 : 1);
        for (std::size_t i = 0; i < rowBytes; ++i) {
            const std::uint8_t reference =
                useUp ? (y > 0 ? row[i - rowBytes] : std::uint8_t{0}) : (i >= step ? row[i - step] : std::uint8_t{0});
            out[i + 1] = static_cast<char>(static_cast<std::uint8_t>(row[i] - reference));
        }
    }

    // zlib: one deflate block with the fixed codes, runs of a byte as matches at distance 1.
    // At most 9 bits per byte (a literal of 144 or more), and the block's header and end.
    std::string zlib(2 + filtered.size() * 9 / 8 + 16, '\0');
    zlib[0] = '\x78';
    zlib[1] = '\x01';
    {
        BitWriter writer(zlib.data() + 2);
        writer.bits(1, 1); // the last block
        writer.bits(1, 2); // fixed Huffman codes
        const char* data = filtered.data();
        const std::size_t size = filtered.size();
        for (std::size_t i = 0; i < size;) {
            std::size_t run = 0;
            if (i > 0) {
                while (run < 258 && i + run < size && data[i + run] == data[i - 1]) {
                    ++run;
                }
            }
            if (run >= 3) {
                repeatPrevious(writer, static_cast<std::uint32_t>(run));
                i += run;
            } else {
                fixedSymbol(writer, static_cast<std::uint8_t>(data[i]));
                ++i;
            }
        }
        fixedSymbol(writer, 256); // end of block
        zlib.resize(2 + writer.finish());
    }
    // Adler-32, reduced every 5,552 bytes (the most that cannot overflow 32 bits).
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t start = 0; start < filtered.size(); start += 5552) {
        const std::size_t end = std::min(filtered.size(), start + 5552);
        for (std::size_t i = start; i < end; ++i) {
            a += static_cast<std::uint8_t>(filtered.data()[i]);
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    bigEndian(zlib, (b << 16) | a);

    std::string png{"\x89PNG\r\n\x1a\n", 8};
    std::string header;
    bigEndian(header, static_cast<std::uint32_t>(image.width));
    bigEndian(header, static_cast<std::uint32_t>(image.height));
    static constexpr std::array<char, 5> kColorTypes{0, 0, 4, 2, 6}; // grey, grey + alpha, RGB, RGBA by channels
    header += '\x08';
    header += kColorTypes[static_cast<std::size_t>(image.channels)];
    header += std::string(3, '\0'); // deflate, adaptive filtering, no interlace
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    return png;
}

assets::ImageData pictureImage(const PicturePayload& payload) {
    if (payload.width < 1 || payload.height < 1 || payload.width > kMaxPictureSide ||
        payload.height > kMaxPictureSide) {
        return {};
    }
    if (payload.shape == PictureShape::Cartridge) {
        return cartridgeImage(payload);
    }
    return assets::convertChannels(
        assets::makeCoverPlaceholder(payload.width, payload.height, payload.title, payload.seed), 3);
}

Expected<std::string> pictureBytes(const PicturePayload& payload) {
    if (payload.damage == PictureDamage::NotAnImage) {
        return fmt::format("Not an image: the cover of \"{}\" is this line of text.\n", payload.title);
    }
    const assets::ImageData image = pictureImage(payload);
    if (!image.isValid()) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("a picture of {}x{} pixels (each side 1..{})",
                                                                 payload.width, payload.height, kMaxPictureSide));
    }
    if (payload.format == PictureFormat::Jpeg && payload.shape == PictureShape::Cartridge) {
        return makeError(ErrorCode::InvalidArgument, "a cartridge picture keeps its transparency: PNG only");
    }
    std::string bytes;
    if (payload.format == PictureFormat::Png && payload.quickPng) {
        bytes = encodeQuickPng(image);
    } else {
        auto encoded = payload.format == PictureFormat::Png ? assets::encodePng(image)
                                                            : assets::encodeJpeg(image, payload.quality);
        if (!encoded) {
            return propagate(encoded.error());
        }
        bytes = toBytes(*encoded);
    }
    if (payload.damage == PictureDamage::Truncated) {
        bytes.resize(bytes.size() / 2);
    }
    return bytes;
}

media::TestVideoSpec defaultClipSpec() noexcept {
    media::TestVideoSpec spec;
    spec.width = 640;
    spec.height = 480;
    spec.frameRate = 30;
    spec.durationSeconds = 4.0;
    spec.codec = media::TestVideoCodec::Mpeg4;
    spec.quantizer = 5;
    spec.keyFrameInterval = 30;
    spec.bFrames = 2;
    spec.audioSeconds = 0.0;
    return spec;
}

Status writeClip(const std::filesystem::path& target, const ClipPayload& payload) {
    const std::string context = core::pathToUtf8Lossy(target);
    const std::string extension = lowerExtension(core::pathToUtf8Lossy(target.extension()));
    if (payload.damage == ClipDamage::Empty) {
        return media::writeRawFile(target, {});
    }
    if (payload.damage == ClipDamage::None) {
        return media::writeTestVideo(target, payload.spec);
    }
    if (payload.damage == ClipDamage::UnsupportedCodec && extension != ".avi") {
        return makeError(ErrorCode::InvalidArgument, "a clip of an unsupported codec is an .avi file", context);
    }
    // The clean clip beside the target, under a name no media lookup takes, then the damaged copy.
    std::filesystem::path source = target;
    source.replace_filename(core::pathFromUtf8("~source-" + core::pathToUtf8Lossy(target.filename())));
    Status written = media::writeTestVideo(source, payload.spec);
    if (written) {
        switch (payload.damage) {
        case ClipDamage::Truncated:
            written = media::writeTruncatedCopy(source, target, 0.5);
            break;
        case ClipDamage::Corrupt:
            written = media::writeCorruptedCopy(source, target, payload.damageSeed);
            break;
        case ClipDamage::UnsupportedCodec:
            written = media::writeUnknownCodecCopy(source, target);
            break;
        case ClipDamage::None:
        case ClipDamage::Empty:
            break;
        }
    }
    std::error_code ec;
    std::filesystem::remove(source, ec);
    if (written && ec) {
        return makeError(ErrorCode::IoError, "cannot remove the clean clip: " + ec.message(),
                         core::pathToUtf8Lossy(source));
    }
    return written;
}

Expected<PicturePayload> parsePicturePayload(const nlohmann::json& value, std::string_view extension,
                                             std::string_view context) {
    if (auto keys = onlyKeys(value, {"title", "seed", "width", "height", "format", "shape", "damage", "quality"},
                             "cover", context);
        !keys) {
        return propagate(keys.error());
    }
    PicturePayload payload;
    if (!value.contains("title") || !value.at("title").is_string()) {
        return invalid(context, "\"title\" is required, a string");
    }
    payload.title = value.at("title").get<std::string>();
    if (!core::isValidUtf8(payload.title)) {
        return invalid(context, "\"title\" is not UTF-8");
    }
    auto seed = requiredSeed(value, context);
    if (!seed) {
        return propagate(seed.error());
    }
    auto width = intIn(value, "width", 1, kMaxPictureSide, payload.width, context);
    if (!width) {
        return propagate(width.error());
    }
    auto height = intIn(value, "height", 1, kMaxPictureSide, payload.height, context);
    if (!height) {
        return propagate(height.error());
    }
    auto quality = intIn(value, "quality", 1, 100, payload.quality, context);
    if (!quality) {
        return propagate(quality.error());
    }
    auto format = optionalWord(value, "format", context);
    if (!format) {
        return propagate(format.error());
    }
    auto shape = optionalWord(value, "shape", context);
    if (!shape) {
        return propagate(shape.error());
    }
    auto damage = optionalWord(value, "damage", context);
    if (!damage) {
        return propagate(damage.error());
    }
    payload.seed = *seed;
    payload.width = *width;
    payload.height = *height;
    payload.quality = *quality;

    const std::string ext = lowerExtension(extension);
    std::string formatName = *format;
    if (formatName.empty()) {
        formatName = ext == ".png" ? "png" : ext == ".jpg" || ext == ".jpeg" ? "jpeg" : "";
    }
    if (formatName == "png") {
        payload.format = PictureFormat::Png;
    } else if (formatName == "jpeg") {
        payload.format = PictureFormat::Jpeg;
    } else if (formatName.empty()) {
        return invalid(context, "\"format\" is required for a file that is neither .png nor .jpg");
    } else {
        return invalid(context, "\"format\" must be \"png\" or \"jpeg\"");
    }

    if (shape->empty() || *shape == "box") {
        payload.shape = PictureShape::Box;
    } else if (*shape == "cartridge") {
        payload.shape = PictureShape::Cartridge;
    } else {
        return invalid(context, "\"shape\" must be \"box\" or \"cartridge\"");
    }
    if (payload.shape == PictureShape::Cartridge && payload.format != PictureFormat::Png) {
        return invalid(context, "a \"cartridge\" picture is a PNG (it keeps its transparency)");
    }

    if (damage->empty()) {
        payload.damage = PictureDamage::None;
    } else if (*damage == "truncated") {
        payload.damage = PictureDamage::Truncated;
    } else if (*damage == "not-an-image") {
        payload.damage = PictureDamage::NotAnImage;
    } else {
        return invalid(context, "\"damage\" must be \"truncated\" or \"not-an-image\"");
    }
    return payload;
}

Expected<ClipPayload> parseClipPayload(const nlohmann::json& value, std::string_view extension,
                                       std::string_view context) {
    if (auto keys = onlyKeys(value, {"seed", "seconds", "width", "height", "frameRate", "codec", "quantizer", "damage"},
                             "video", context);
        !keys) {
        return propagate(keys.error());
    }
    ClipPayload payload;
    auto seed = requiredSeed(value, context);
    if (!seed) {
        return propagate(seed.error());
    }
    payload.spec.seed = *seed;
    payload.damageSeed = *seed;
    auto width = intIn(value, "width", 16, 4096, payload.spec.width, context);
    if (!width) {
        return propagate(width.error());
    }
    auto height = intIn(value, "height", 16, 4096, payload.spec.height, context);
    if (!height) {
        return propagate(height.error());
    }
    auto frameRate = intIn(value, "frameRate", 1, 240, payload.spec.frameRate, context);
    if (!frameRate) {
        return propagate(frameRate.error());
    }
    auto quantizer = intIn(value, "quantizer", 2, 31, payload.spec.quantizer, context);
    if (!quantizer) {
        return propagate(quantizer.error());
    }
    if (*width % 2 != 0 || *height % 2 != 0) {
        return invalid(context, "\"width\" and \"height\" of a clip must be even");
    }
    payload.spec.width = *width;
    payload.spec.height = *height;
    payload.spec.frameRate = *frameRate;
    payload.spec.quantizer = *quantizer;
    payload.spec.keyFrameInterval = *frameRate;
    if (value.contains("seconds")) {
        const nlohmann::json& seconds = value.at("seconds");
        if (!seconds.is_number() || !(seconds.get<double>() > 0.0) || seconds.get<double>() > 60.0) {
            return invalid(context, "\"seconds\" must be a number in (0, 60]");
        }
        payload.spec.durationSeconds = seconds.get<double>();
    }

    auto codec = optionalWord(value, "codec", context);
    auto damage = optionalWord(value, "damage", context);
    if (!codec) {
        return propagate(codec.error());
    }
    if (!damage) {
        return propagate(damage.error());
    }
    const std::string ext = lowerExtension(extension);
    if (codec->empty() || *codec == "mpeg4") {
        payload.spec.codec = media::TestVideoCodec::Mpeg4;
    } else if (*codec == "mjpeg") {
        payload.spec.codec = media::TestVideoCodec::Mjpeg;
        if (ext == ".mp4") {
            return invalid(context, "a Motion JPEG clip goes into an .mkv or .avi file");
        }
    } else {
        return invalid(context, "\"codec\" must be \"mpeg4\" or \"mjpeg\"");
    }
    if (ext != ".mp4" && ext != ".mkv" && ext != ".avi") {
        return invalid(context, "a clip is an .mp4, .mkv or .avi file");
    }

    if (damage->empty()) {
        payload.damage = ClipDamage::None;
    } else if (*damage == "empty") {
        payload.damage = ClipDamage::Empty;
    } else if (*damage == "truncated") {
        payload.damage = ClipDamage::Truncated;
    } else if (*damage == "corrupt") {
        payload.damage = ClipDamage::Corrupt;
    } else if (*damage == "unsupported-codec") {
        payload.damage = ClipDamage::UnsupportedCodec;
        if (ext != ".avi") {
            return invalid(context, "a clip of an unsupported codec is an .avi file");
        }
    } else {
        return invalid(context, "\"damage\" must be \"empty\", \"truncated\", \"corrupt\" or \"unsupported-codec\"");
    }
    return payload;
}

} // namespace rv::test
