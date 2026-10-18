#include <rv/render/gpu_memory.hpp>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace rv::gl {
namespace {

static_assert(std::is_trivially_destructible_v<GpuMemoryLedger>,
              "allocations released during static destruction must find the ledger intact");

constexpr std::size_t index(GpuMemoryKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

/// Bytes per texel of an uncompressed format; 0 for a block-compressed one.
constexpr std::size_t bytesPerTexel(GpuFormat format) noexcept {
    switch (format) {
    case GpuFormat::R8:
        return 1;
    case GpuFormat::Rgba8:
    case GpuFormat::Srgb8Alpha8:
    case GpuFormat::Rg16F:
    case GpuFormat::Depth24Stencil8:
    case GpuFormat::Depth32F:
        return 4;
    case GpuFormat::Rgba16F:
        return 8;
    case GpuFormat::Bc1:
        return 0;
    }
    return 0;
}

constexpr std::size_t kBc1BlockBytes = 8;
constexpr int kBc1BlockSide = 4;

} // namespace

std::string_view toString(GpuMemoryKind kind) noexcept {
    switch (kind) {
    case GpuMemoryKind::ProceduralSurfaces:
        return "proceduralSurfaces";
    case GpuMemoryKind::CollectionArrays:
        return "collectionArrays";
    case GpuMemoryKind::HeroTextures:
        return "heroTextures";
    case GpuMemoryKind::Video:
        return "video";
    case GpuMemoryKind::Targets:
        return "targets";
    case GpuMemoryKind::SceneTextures:
        return "sceneTextures";
    case GpuMemoryKind::Buffers:
        return "buffers";
    }
    return "sceneTextures";
}

std::optional<GpuMemoryKind> gpuMemoryKindFromString(std::string_view name) noexcept {
    for (const GpuMemoryKind kind : kGpuMemoryKinds) {
        if (toString(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view displayName(GpuMemoryKind kind) noexcept {
    switch (kind) {
    case GpuMemoryKind::ProceduralSurfaces:
        return "Surfaces";
    case GpuMemoryKind::CollectionArrays:
        return "Arrays";
    case GpuMemoryKind::HeroTextures:
        return "Hero";
    case GpuMemoryKind::Video:
        return "Video";
    case GpuMemoryKind::Targets:
        return "Targets";
    case GpuMemoryKind::SceneTextures:
        return "Textures";
    case GpuMemoryKind::Buffers:
        return "Buffers";
    }
    return "Textures";
}

std::size_t textureLevelBytes(GpuFormat format, glm::ivec2 size) noexcept {
    if (size.x <= 0 || size.y <= 0) {
        return 0;
    }
    if (format == GpuFormat::Bc1) {
        const auto blocksX = static_cast<std::size_t>((size.x + kBc1BlockSide - 1) / kBc1BlockSide);
        const auto blocksY = static_cast<std::size_t>((size.y + kBc1BlockSide - 1) / kBc1BlockSide);
        return blocksX * blocksY * kBc1BlockBytes;
    }
    return static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * bytesPerTexel(format);
}

std::size_t textureBytes(GpuFormat format, glm::ivec2 size, int levels, int layers) noexcept {
    if (size.x <= 0 || size.y <= 0 || levels <= 0 || layers <= 0) {
        return 0;
    }
    std::size_t bytes = 0;
    glm::ivec2 level = size;
    for (int i = 0; i < levels; ++i) {
        bytes += textureLevelBytes(format, level);
        level = glm::ivec2(std::max(level.x / 2, 1), std::max(level.y / 2, 1));
    }
    return bytes * static_cast<std::size_t>(layers);
}

void GpuMemoryLedger::add(GpuMemoryKind kind, std::uint64_t bytes) noexcept {
    GpuMemoryUsage& usage = m_kinds[index(kind)];
    usage.bytes += bytes;
    usage.peakBytes = std::max(usage.peakBytes, usage.bytes);
    ++usage.allocations;
    m_totalBytes += bytes;
    m_peakTotalBytes = std::max(m_peakTotalBytes, m_totalBytes);
}

void GpuMemoryLedger::remove(GpuMemoryKind kind, std::uint64_t bytes) noexcept {
    GpuMemoryUsage& usage = m_kinds[index(kind)];
    const std::uint64_t released = std::min(usage.bytes, bytes);
    usage.bytes -= released;
    usage.allocations -= std::min<std::uint64_t>(usage.allocations, 1);
    m_totalBytes -= std::min(m_totalBytes, released);
}

const GpuMemoryUsage& GpuMemoryLedger::usage(GpuMemoryKind kind) const noexcept {
    return m_kinds[index(kind)];
}

GpuMemoryLedger& gpuMemoryLedger() noexcept {
    static GpuMemoryLedger ledger;
    return ledger;
}

GpuMemoryAllocation::GpuMemoryAllocation(GpuMemoryKind kind, std::uint64_t bytes, GpuMemoryLedger& ledger) noexcept
    : m_ledger(&ledger), m_kind(kind), m_bytes(bytes) {
    m_ledger->add(m_kind, m_bytes);
}

GpuMemoryAllocation::GpuMemoryAllocation(GpuMemoryAllocation&& other) noexcept
    : m_ledger(std::exchange(other.m_ledger, nullptr)), m_kind(other.m_kind),
      m_bytes(std::exchange(other.m_bytes, std::uint64_t{0})) {}

GpuMemoryAllocation& GpuMemoryAllocation::operator=(GpuMemoryAllocation&& other) noexcept {
    if (this != &other) {
        reset();
        m_ledger = std::exchange(other.m_ledger, nullptr);
        m_kind = other.m_kind;
        m_bytes = std::exchange(other.m_bytes, std::uint64_t{0});
    }
    return *this;
}

GpuMemoryAllocation::~GpuMemoryAllocation() {
    reset();
}

void GpuMemoryAllocation::reset() noexcept {
    if (m_ledger != nullptr) {
        m_ledger->remove(m_kind, m_bytes);
    }
    m_ledger = nullptr;
    m_bytes = 0;
}

void GpuMemoryAllocation::setKind(GpuMemoryKind kind) noexcept {
    if (m_ledger == nullptr || kind == m_kind) {
        return;
    }
    m_ledger->remove(m_kind, m_bytes);
    m_kind = kind;
    m_ledger->add(m_kind, m_bytes);
}

} // namespace rv::gl
