#include <rv/assets/rvtex.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <type_traits>

#include <rv/assets/bc1.hpp>

namespace rv::assets {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {0x89, 'R', 'V', 'T', 'X', 0x0D, 0x0A, 0x1A};
constexpr std::uint8_t kFlagHasSource = 0x01;

void put8(std::vector<std::byte>& out, std::size_t at, std::uint8_t value) {
    out[at] = static_cast<std::byte>(value);
}

template <class T>
void putLe(std::vector<std::byte>& out, std::size_t at, T value) {
    auto bits = static_cast<std::make_unsigned_t<T>>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[at + i] = static_cast<std::byte>(bits & 0xFFU);
        bits = static_cast<std::make_unsigned_t<T>>(bits >> 8);
    }
}

template <class T>
T getLe(std::span<const std::byte> in, std::size_t at) {
    std::make_unsigned_t<T> bits = 0;
    for (std::size_t i = sizeof(T); i-- > 0;) {
        bits = static_cast<std::make_unsigned_t<T>>((bits << 8) | static_cast<std::uint8_t>(in[at + i]));
    }
    return static_cast<T>(bits);
}

bool validClass(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(TextureClass::Front) ||
           value == static_cast<std::uint8_t>(TextureClass::Spine);
}

bool validFormat(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(TextureFormat::Bc1Srgb) ||
           value == static_cast<std::uint8_t>(TextureFormat::Rgba8Srgb);
}

/// The chain's shape check shared by the writer and the reader.
const char* shapeProblem(const RvTexture& texture) noexcept {
    const LayerSize size = layerSize(texture.textureClass, texture.format);
    if (size.width == 0 || texture.width != size.width || texture.height != size.height) {
        return "the size does not match the class and format";
    }
    if (texture.levels.size() != static_cast<std::size_t>(mipLevelCount(texture.width, texture.height))) {
        return "the mip chain is not complete";
    }
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        const TextureLevel& data = texture.levels[level];
        const int width = mipLevelSide(texture.width, static_cast<int>(level));
        const int height = mipLevelSide(texture.height, static_cast<int>(level));
        if (data.width != width || data.height != height || data.data.size() != levelBytes(texture.format, width, height)) {
            return "a mip level has the wrong size";
        }
    }
    return nullptr;
}

Error invalid(std::string message, std::string_view context) {
    return Error{ErrorCode::ValidationFailed, "invalid .rvtex file: " + std::move(message), std::string(context)};
}

} // namespace

std::string_view toString(TextureClass textureClass) noexcept {
    switch (textureClass) {
    case TextureClass::Front:
        return "front";
    case TextureClass::Spine:
        return "spine";
    }
    return "unknown";
}

std::string_view toString(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Bc1Srgb:
        return "bc1";
    case TextureFormat::Rgba8Srgb:
        return "rgba8";
    }
    return "unknown";
}

LayerSize layerSize(TextureClass textureClass, TextureFormat format) noexcept {
    LayerSize size;
    switch (textureClass) {
    case TextureClass::Front:
        size = {256, 188};
        break;
    case TextureClass::Spine:
        size = {40, 256};
        break;
    default:
        return {};
    }
    switch (format) {
    case TextureFormat::Bc1Srgb:
        return size;
    case TextureFormat::Rgba8Srgb:
        return {size.width / 2, size.height / 2};
    }
    return {};
}

std::size_t levelBytes(TextureFormat format, int width, int height) noexcept {
    if (width < 1 || height < 1) {
        return 0;
    }
    if (format == TextureFormat::Bc1Srgb) {
        return bc1LevelBytes(width, height);
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}

std::size_t layerBytes(TextureClass textureClass, TextureFormat format) noexcept {
    const LayerSize size = layerSize(textureClass, format);
    std::size_t bytes = 0;
    for (int level = 0; level < mipLevelCount(size.width, size.height); ++level) {
        bytes += levelBytes(format, mipLevelSide(size.width, level), mipLevelSide(size.height, level));
    }
    return bytes;
}

std::size_t RvTexture::payloadBytes() const noexcept {
    std::size_t bytes = 0;
    for (const TextureLevel& level : levels) {
        bytes += level.data.size();
    }
    return bytes;
}

Expected<RvTexture> makeLayerTexture(const ImageData& picture, TextureClass textureClass, TextureFormat format,
                                     Rgb8 dominant) {
    const LayerSize size = layerSize(textureClass, format);
    if (!picture.isValid() || picture.channels != 4 || picture.width != size.width || picture.height != size.height) {
        return makeError(ErrorCode::InvalidArgument,
                         "a " + std::string(toString(textureClass)) + " layer in " + std::string(toString(format)) +
                             " needs a " + std::to_string(size.width) + "x" + std::to_string(size.height) +
                             " RGBA picture, got " + std::to_string(picture.width) + "x" +
                             std::to_string(picture.height) + " with " + std::to_string(picture.channels) +
                             " channels",
                         "layer");
    }
    RvTexture texture;
    texture.textureClass = textureClass;
    texture.format = format;
    texture.width = size.width;
    texture.height = size.height;
    texture.dominant = dominant;
    std::vector<ImageData> chain = buildMipChain(picture);
    texture.levels.reserve(chain.size());
    for (ImageData& level : chain) {
        TextureLevel out;
        out.width = level.width;
        out.height = level.height;
        if (format == TextureFormat::Bc1Srgb) {
            out.data = encodeBc1(level);
        } else {
            out.data.resize(level.pixels.size());
            std::memcpy(out.data.data(), level.pixels.data(), level.pixels.size());
        }
        texture.levels.push_back(std::move(out));
    }
    return texture;
}

ImageData decodeLevel(const RvTexture& texture, std::size_t level) {
    if (level >= texture.levels.size()) {
        return {};
    }
    const TextureLevel& data = texture.levels[level];
    if (texture.format == TextureFormat::Bc1Srgb) {
        return decodeBc1(data.data, data.width, data.height);
    }
    if (data.data.size() != levelBytes(TextureFormat::Rgba8Srgb, data.width, data.height)) {
        return {};
    }
    ImageData image;
    image.width = data.width;
    image.height = data.height;
    image.channels = 4;
    image.pixels.resize(data.data.size());
    std::memcpy(image.pixels.data(), data.data.data(), data.data.size());
    return image;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

Expected<std::vector<std::byte>> serializeRvtex(const RvTexture& texture, const core::Sha256Digest& key) {
    if (const char* problem = shapeProblem(texture)) {
        return makeError(ErrorCode::InvalidArgument, std::string("cannot write the layer: ") + problem, ".rvtex");
    }
    const std::size_t payload = texture.payloadBytes();
    std::vector<std::byte> out(kRvtexHeaderBytes + payload);
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        put8(out, i, kMagic[i]);
    }
    putLe<std::uint32_t>(out, 8, kRvtexFileVersion);
    putLe<std::uint32_t>(out, 12, kCoverPipelineVersion);
    put8(out, 16, static_cast<std::uint8_t>(texture.textureClass));
    put8(out, 17, static_cast<std::uint8_t>(texture.format));
    put8(out, 18, static_cast<std::uint8_t>(texture.levels.size()));
    put8(out, 19, texture.source ? kFlagHasSource : std::uint8_t{0});
    putLe<std::uint16_t>(out, 20, static_cast<std::uint16_t>(texture.width));
    putLe<std::uint16_t>(out, 22, static_cast<std::uint16_t>(texture.height));
    put8(out, 24, texture.dominant.r);
    put8(out, 25, texture.dominant.g);
    put8(out, 26, texture.dominant.b);
    putLe<std::uint32_t>(out, 28, static_cast<std::uint32_t>(payload));
    putLe<std::uint64_t>(out, 32, texture.source ? texture.source->size : 0);
    putLe<std::int64_t>(out, 40, texture.source ? texture.source->lastWriteTime : 0);
    for (std::size_t i = 0; i < key.size(); ++i) {
        put8(out, 48 + i, key[i]);
    }
    std::size_t at = kRvtexHeaderBytes;
    for (const TextureLevel& level : texture.levels) {
        std::memcpy(out.data() + at, level.data.data(), level.data.size());
        at += level.data.size();
    }
    putLe<std::uint64_t>(out, 80, fnv1a64(std::span<const std::byte>(out).subspan(kRvtexHeaderBytes)));
    putLe<std::uint64_t>(out, 88, fnv1a64(std::span<const std::byte>(out).first(88)));
    return out;
}

Expected<RvTexture> parseRvtex(std::span<const std::byte> bytes, const core::Sha256Digest* key,
                               std::string_view context) {
    if (bytes.size() < kRvtexHeaderBytes) {
        return propagate(invalid("shorter than its header (" + std::to_string(bytes.size()) + " bytes)", context));
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (static_cast<std::uint8_t>(bytes[i]) != kMagic[i]) {
            return propagate(invalid("not an .rvtex file (magic)", context));
        }
    }
    if (getLe<std::uint64_t>(bytes, 88) != fnv1a64(bytes.first(88))) {
        return propagate(invalid("the header's checksum does not match", context));
    }
    const auto fileVersion = getLe<std::uint32_t>(bytes, 8);
    if (fileVersion != kRvtexFileVersion) {
        return propagate(invalid("file version " + std::to_string(fileVersion), context));
    }
    const auto pipelineVersion = getLe<std::uint32_t>(bytes, 12);
    if (pipelineVersion != kCoverPipelineVersion) {
        return propagate(invalid("pipeline version " + std::to_string(pipelineVersion), context));
    }
    const auto classValue = static_cast<std::uint8_t>(bytes[16]);
    const auto formatValue = static_cast<std::uint8_t>(bytes[17]);
    if (!validClass(classValue) || !validFormat(formatValue)) {
        return propagate(invalid("unknown class or format", context));
    }
    const auto flags = static_cast<std::uint8_t>(bytes[19]);
    if ((flags & ~kFlagHasSource) != 0 || bytes[27] != std::byte{0}) {
        return propagate(invalid("reserved bits set", context));
    }
    if (key != nullptr && std::memcmp(key->data(), bytes.data() + 48, key->size()) != 0) {
        return propagate(invalid("the entry belongs to another key", context));
    }
    RvTexture texture;
    texture.textureClass = static_cast<TextureClass>(classValue);
    texture.format = static_cast<TextureFormat>(formatValue);
    texture.width = getLe<std::uint16_t>(bytes, 20);
    texture.height = getLe<std::uint16_t>(bytes, 22);
    texture.dominant = {static_cast<std::uint8_t>(bytes[24]), static_cast<std::uint8_t>(bytes[25]),
                        static_cast<std::uint8_t>(bytes[26])};
    if ((flags & kFlagHasSource) != 0) {
        texture.source = SourceStamp{getLe<std::uint64_t>(bytes, 32), getLe<std::int64_t>(bytes, 40)};
    } else if (getLe<std::uint64_t>(bytes, 32) != 0 || getLe<std::int64_t>(bytes, 40) != 0) {
        return propagate(invalid("a source stamp without its flag", context));
    }
    const LayerSize size = layerSize(texture.textureClass, texture.format);
    const auto mipCount = static_cast<std::uint8_t>(bytes[18]);
    if (texture.width != size.width || texture.height != size.height ||
        mipCount != mipLevelCount(texture.width, texture.height)) {
        return propagate(invalid("the size or mip count does not match the class and format", context));
    }
    const auto payload = getLe<std::uint32_t>(bytes, 28);
    if (payload != layerBytes(texture.textureClass, texture.format) ||
        bytes.size() != kRvtexHeaderBytes + static_cast<std::size_t>(payload)) {
        return propagate(invalid("the payload is " + std::to_string(bytes.size() - kRvtexHeaderBytes) +
                                     " bytes, the header says " + std::to_string(payload),
                                 context));
    }
    if (getLe<std::uint64_t>(bytes, 80) != fnv1a64(bytes.subspan(kRvtexHeaderBytes))) {
        return propagate(invalid("the payload's checksum does not match", context));
    }
    std::size_t at = kRvtexHeaderBytes;
    texture.levels.reserve(mipCount);
    for (int level = 0; level < mipCount; ++level) {
        TextureLevel data;
        data.width = mipLevelSide(texture.width, level);
        data.height = mipLevelSide(texture.height, level);
        const std::size_t length = levelBytes(texture.format, data.width, data.height);
        data.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                         bytes.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        texture.levels.push_back(std::move(data));
    }
    return texture;
}

} // namespace rv::assets
