#include <rv/render/texture_array_layout.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include <fmt/format.h>

#include <rv/render/texture_memory.hpp>

namespace rv::gl {
namespace {

constexpr std::size_t kBc1BlockBytes = 8;

glm::ivec2 fullLayerSize(CollectionClass cls) noexcept {
    return cls == CollectionClass::Front ? kFrontLayerSize : kSpineLayerSize;
}

/// round(value * maxOut / 255) for an 8-bit value.
std::uint16_t quantize(std::uint8_t value, int maxOut) noexcept {
    return static_cast<std::uint16_t>((value * maxOut * 2 + 255) / (2 * 255));
}

} // namespace

std::string_view toString(CollectionClass cls) noexcept {
    return cls == CollectionClass::Front ? "front" : "spine";
}

std::string_view toString(CollectionArrayFormat format) noexcept {
    return format == CollectionArrayFormat::Bc1Srgb ? "bc1-srgb" : "rgba8-srgb";
}

GpuFormat gpuFormatOf(CollectionArrayFormat format) noexcept {
    return format == CollectionArrayFormat::Bc1Srgb ? GpuFormat::Bc1 : GpuFormat::Srgb8Alpha8;
}

CollectionFormatChoice chooseCollectionArrayFormat(bool s3tcSrgbAvailable, bool compressionDisabled) noexcept {
    if (compressionDisabled) {
        return {CollectionArrayFormat::Rgba8Srgb, "--no-texture-compression"};
    }
    if (!s3tcSrgbAvailable) {
        return {CollectionArrayFormat::Rgba8Srgb, "GL_EXT_texture_compression_s3tc or GL_EXT_texture_sRGB missing"};
    }
    return {CollectionArrayFormat::Bc1Srgb, "GL_EXT_texture_compression_s3tc and GL_EXT_texture_sRGB present"};
}

glm::ivec2 layerSize(CollectionClass cls, CollectionArrayFormat format) noexcept {
    const glm::ivec2 full = fullLayerSize(cls);
    return format == CollectionArrayFormat::Bc1Srgb ? full : full / 2;
}

int layerLevelCount(CollectionClass cls, CollectionArrayFormat format) noexcept {
    return fullMipLevelCount(layerSize(cls, format));
}

glm::ivec2 mipLevelSize(glm::ivec2 base, int level) noexcept {
    if (level < 0 || base.x <= 0 || base.y <= 0) {
        return glm::ivec2(0);
    }
    const int shift = std::min(level, 30);
    return glm::ivec2(std::max(base.x >> shift, 1), std::max(base.y >> shift, 1));
}

std::size_t layerLevelBytes(CollectionClass cls, CollectionArrayFormat format, int level) noexcept {
    if (level < 0 || level >= layerLevelCount(cls, format)) {
        return 0;
    }
    return textureLevelBytes(gpuFormatOf(format), mipLevelSize(layerSize(cls, format), level));
}

std::size_t layerBytes(CollectionClass cls, CollectionArrayFormat format) noexcept {
    return textureBytes(gpuFormatOf(format), layerSize(cls, format), layerLevelCount(cls, format));
}

std::size_t arrayBytes(CollectionClass cls, CollectionArrayFormat format, int layers) noexcept {
    return textureBytes(gpuFormatOf(format), layerSize(cls, format), layerLevelCount(cls, format), layers);
}

int arrayLayersFor(std::size_t items) noexcept {
    const std::size_t layers = std::min<std::size_t>(items, kMaxItemsPerArray) + 1;
    const std::size_t rounded = (layers + kArrayLayerGranularity - 1) / kArrayLayerGranularity * kArrayLayerGranularity;
    return static_cast<int>(std::min<std::size_t>(rounded, kMaxArrayLayers));
}

std::vector<int> planArrayLayers(std::size_t items) {
    std::vector<int> arrays;
    std::size_t left = items;
    while (left > kMaxItemsPerArray) {
        arrays.push_back(kMaxArrayLayers);
        left -= kMaxItemsPerArray;
    }
    if (left > 0 || arrays.empty()) {
        arrays.push_back(arrayLayersFor(left));
    }
    return arrays;
}

std::uint16_t toRgb565(Rgb8 color) noexcept {
    return static_cast<std::uint16_t>((quantize(color.r, 31) << 11) | (quantize(color.g, 63) << 5) |
                                      quantize(color.b, 31));
}

Rgb8 fromRgb565(std::uint16_t rgb565) noexcept {
    const int r = (rgb565 >> 11) & 0x1F;
    const int g = (rgb565 >> 5) & 0x3F;
    const int b = rgb565 & 0x1F;
    return Rgb8{static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
                static_cast<std::uint8_t>((b << 3) | (b >> 2))};
}

Status validateLayerImage(const LayerImage& image, CollectionClass cls, CollectionArrayFormat format) {
    const std::string context = fmt::format("{} layer", toString(cls));
    if (image.format != format) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("the layer is {}, the arrays are {}", toString(image.format), toString(format)),
                         context);
    }
    const int levels = layerLevelCount(cls, format);
    if (static_cast<int>(image.levels.size()) != levels) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("{} level(s) instead of {}", image.levels.size(), levels), context);
    }
    for (int level = 0; level < levels; ++level) {
        const std::size_t expected = layerLevelBytes(cls, format, level);
        const std::size_t actual = image.levels[static_cast<std::size_t>(level)].size();
        if (actual != expected) {
            const glm::ivec2 size = mipLevelSize(layerSize(cls, format), level);
            return makeError(
                ErrorCode::InvalidArgument,
                fmt::format("level {} ({}x{}) holds {} bytes instead of {}", level, size.x, size.y, actual, expected),
                context);
        }
    }
    return ok();
}

LayerImage makeSolidLayer(CollectionClass cls, CollectionArrayFormat format, Rgb8 color) {
    LayerImage image;
    image.format = format;
    const int levels = layerLevelCount(cls, format);
    image.levels.resize(static_cast<std::size_t>(levels));
    if (format == CollectionArrayFormat::Bc1Srgb) {
        // Both endpoints equal and every 2-bit index 0: each texel is endpoint 0, whichever of the two BC1 modes the
        // endpoints' order selects.
        const std::uint16_t endpoint = toRgb565(color);
        std::array<std::byte, kBc1BlockBytes> block{};
        block[0] = static_cast<std::byte>(endpoint & 0xFF);
        block[1] = static_cast<std::byte>(endpoint >> 8);
        block[2] = block[0];
        block[3] = block[1];
        for (int level = 0; level < levels; ++level) {
            std::vector<std::byte>& bytes = image.levels[static_cast<std::size_t>(level)];
            bytes.resize(layerLevelBytes(cls, format, level));
            for (std::size_t offset = 0; offset + kBc1BlockBytes <= bytes.size(); offset += kBc1BlockBytes) {
                std::memcpy(bytes.data() + offset, block.data(), kBc1BlockBytes);
            }
        }
        return image;
    }
    const std::array<std::byte, 4> texel{static_cast<std::byte>(color.r), static_cast<std::byte>(color.g),
                                         static_cast<std::byte>(color.b), std::byte{0xFF}};
    for (int level = 0; level < levels; ++level) {
        std::vector<std::byte>& bytes = image.levels[static_cast<std::size_t>(level)];
        bytes.resize(layerLevelBytes(cls, format, level));
        for (std::size_t offset = 0; offset + texel.size() <= bytes.size(); offset += texel.size()) {
            std::memcpy(bytes.data() + offset, texel.data(), texel.size());
        }
    }
    return image;
}

LayerAllocator::LayerAllocator(int layers) : m_layers(static_cast<std::size_t>(std::max(layers, 1))) {}

std::uint32_t LayerAllocator::assign(std::uint32_t layer, std::string_view key) {
    Layer& entry = m_layers[layer];
    entry.state = State::InUse;
    entry.resident = false;
    ++entry.generation;
    entry.releasedAt = 0;
    entry.key = std::string(key);
    m_keys.emplace(entry.key, layer);
    ++m_used;
    return entry.generation;
}

std::optional<LayerAllocator::Acquired> LayerAllocator::acquire(std::string_view key) {
    if (const auto found = m_keys.find(key); found != m_keys.end()) {
        Layer& entry = m_layers[found->second];
        if (entry.state == State::Released) {
            entry.state = State::InUse;
            entry.releasedAt = 0;
            --m_released;
            ++m_used;
        }
        return Acquired{found->second, entry.generation, entry.resident, true};
    }
    if (m_nextFree < m_layers.size()) {
        const std::uint32_t layer = m_nextFree++;
        const std::uint32_t generation = assign(layer, key);
        return Acquired{layer, generation, false, false};
    }
    if (m_released == 0) {
        return std::nullopt;
    }
    // The least recently released layer (the earliest release; ties cannot happen, the clock only grows).
    std::uint32_t oldest = 0;
    std::uint64_t oldestAt = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t layer = 1; layer < m_layers.size(); ++layer) {
        const Layer& entry = m_layers[layer];
        if (entry.state == State::Released && entry.releasedAt < oldestAt) {
            oldest = layer;
            oldestAt = entry.releasedAt;
        }
    }
    Layer& evicted = m_layers[oldest];
    m_keys.erase(evicted.key);
    --m_released;
    const std::uint32_t generation = assign(oldest, key);
    return Acquired{oldest, generation, false, false};
}

bool LayerAllocator::release(std::string_view key) {
    const auto found = m_keys.find(key);
    if (found == m_keys.end()) {
        return false;
    }
    Layer& entry = m_layers[found->second];
    if (entry.state != State::InUse) {
        return false;
    }
    entry.state = State::Released;
    entry.releasedAt = ++m_clock;
    --m_used;
    ++m_released;
    return true;
}

bool LayerAllocator::markResident(std::uint32_t layer, std::uint32_t generation) {
    if (layer == kPendingLayer || layer >= m_layers.size()) {
        return false;
    }
    Layer& entry = m_layers[layer];
    if (entry.state == State::Free || entry.generation != generation) {
        return false;
    }
    entry.resident = true;
    return true;
}

void LayerAllocator::grow(int layers) {
    if (layers > capacity()) {
        m_layers.resize(static_cast<std::size_t>(layers));
    }
}

std::optional<std::uint32_t> LayerAllocator::layerOf(std::string_view key) const {
    if (const auto found = m_keys.find(key); found != m_keys.end()) {
        return found->second;
    }
    return std::nullopt;
}

std::uint32_t LayerAllocator::generation(std::uint32_t layer) const noexcept {
    return layer < m_layers.size() ? m_layers[layer].generation : 0;
}

bool LayerAllocator::resident(std::uint32_t layer) const noexcept {
    return layer < m_layers.size() && m_layers[layer].state != State::Free && m_layers[layer].resident;
}

bool LayerAllocator::inUse(std::uint32_t layer) const noexcept {
    return layer < m_layers.size() && m_layers[layer].state == State::InUse;
}

std::size_t LayerAllocator::neverUsedCount() const noexcept {
    return m_layers.size() - m_nextFree;
}

std::size_t LayerAllocator::residentUsedCount() const noexcept {
    std::size_t count = 0;
    for (const Layer& entry : m_layers) {
        count += entry.state == State::InUse && entry.resident ? 1 : 0;
    }
    return count;
}

void UploadBudgetMeter::beginFrame(const UploadBudget& budget) noexcept {
    m_budget = budget;
    m_layers = 0;
}

bool UploadBudgetMeter::mayUploadNext(std::chrono::nanoseconds elapsed) const noexcept {
    if (m_layers == 0) {
        return m_budget.layers > 0;
    }
    return m_layers < m_budget.layers && elapsed + m_average <= m_budget.time;
}

void UploadBudgetMeter::recordLayer(std::chrono::nanoseconds cost) noexcept {
    ++m_layers;
    const std::chrono::nanoseconds measured = std::max(cost, std::chrono::nanoseconds(0));
    m_average = m_average.count() == 0 ? measured : m_average + (measured - m_average) / 4;
}

} // namespace rv::gl
