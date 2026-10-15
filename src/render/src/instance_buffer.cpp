#include <rv/render/instance_buffer.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace rv::gl {
namespace {

constexpr std::size_t kMatrixBytes = sizeof(glm::mat4);
constexpr GLbitfield kMappingFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

/// Generations are unique across every instance buffer, so a mesh attached to one is never mistaken for attached to
/// another.
std::uint64_t nextGeneration() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

} // namespace

InstanceBuffer::InstanceBuffer(InstanceBuffer&& other) noexcept
    : m_buffer(std::move(other.m_buffer)), m_mapped(std::exchange(other.m_mapped, nullptr)),
      m_regionCapacity(std::exchange(other.m_regionCapacity, std::size_t{0})), m_fences(std::move(other.m_fences)),
      m_region(std::exchange(other.m_region, kRegionCount - 1)),
      m_releasePending(std::exchange(other.m_releasePending, false)),
      m_generation(std::exchange(other.m_generation, std::uint64_t{0})), m_debugName(std::move(other.m_debugName)) {}

InstanceBuffer& InstanceBuffer::operator=(InstanceBuffer&& other) noexcept {
    if (this != &other) {
        m_buffer = std::move(other.m_buffer);
        m_mapped = std::exchange(other.m_mapped, nullptr);
        m_regionCapacity = std::exchange(other.m_regionCapacity, std::size_t{0});
        m_fences = std::move(other.m_fences);
        m_region = std::exchange(other.m_region, kRegionCount - 1);
        m_releasePending = std::exchange(other.m_releasePending, false);
        m_generation = std::exchange(other.m_generation, std::uint64_t{0});
        m_debugName = std::move(other.m_debugName);
    }
    return *this;
}

Expected<InstanceBuffer> InstanceBuffer::create(std::size_t regionCapacity, std::string_view debugName) {
    InstanceBuffer instances;
    instances.m_debugName = std::string(debugName);
    if (auto allocated = instances.allocate(std::max<std::size_t>(regionCapacity, 1)); !allocated) {
        return propagate(allocated.error());
    }
    return instances;
}

Status InstanceBuffer::allocate(std::size_t regionCapacity) {
    if (regionCapacity > kMaxRegionCapacity) {
        return makeError(
            ErrorCode::OutOfRange,
            fmt::format("{} instances exceed the limit of {} per view", regionCapacity, kMaxRegionCapacity),
            m_debugName);
    }
    auto buffer = Buffer::create(kRegionCount * regionCapacity * kMatrixBytes, kMappingFlags, m_debugName);
    if (!buffer) {
        return propagate(buffer.error());
    }
    void* mapped = glMapNamedBufferRange(buffer->id(), 0, static_cast<GLsizeiptr>(buffer->size()), kMappingFlags);
    if (mapped == nullptr) {
        return makeError(ErrorCode::GraphicsError, "the instance buffer cannot be mapped", m_debugName);
    }
    // The previous buffer (deleted, and so unmapped, by the assignment) stays alive in the driver until the draws that
    // read it complete; its fences no longer guard anything.
    m_buffer = std::move(*buffer);
    m_mapped = static_cast<std::byte*>(mapped);
    m_regionCapacity = regionCapacity;
    for (FenceSync& fence : m_fences) {
        fence.reset();
    }
    m_region = kRegionCount - 1;
    m_releasePending = false;
    m_generation = nextGeneration();
    return ok();
}

Expected<std::uint32_t> InstanceBuffer::write(std::span<const glm::mat4> transforms) {
    if (!m_buffer.valid() || m_mapped == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "cannot write to an empty instance buffer", m_debugName);
    }
    if (transforms.empty()) {
        return 0u;
    }
    release(); // a write that was not released still guards its region
    if (transforms.size() > m_regionCapacity) {
        // Doubling keeps reallocations rare; allocate() refuses more than the maximum.
        const std::size_t grown = std::max(transforms.size(), std::min(2 * m_regionCapacity, kMaxRegionCapacity));
        if (auto allocated = allocate(grown); !allocated) {
            return propagate(allocated.error());
        }
        spdlog::info("Instance buffer '{}' grown to {} regions of {} matrices ({} KiB)", m_debugName, kRegionCount,
                     m_regionCapacity, m_buffer.size() / 1024);
    }
    const std::size_t region = (m_region + 1) % kRegionCount;
    if (!m_fences[region].clientWait(kFenceTimeoutNanoseconds)) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("the GPU did not release instance region {} within {} ms", region,
                                     kFenceTimeoutNanoseconds / 1'000'000),
                         m_debugName);
    }
    m_fences[region].reset();
    const std::size_t first = region * m_regionCapacity;
    std::memcpy(m_mapped + first * kMatrixBytes, transforms.data(), transforms.size_bytes());
    m_region = region;
    m_releasePending = true;
    return static_cast<std::uint32_t>(first);
}

void InstanceBuffer::release() {
    if (m_releasePending) {
        m_fences[m_region] = FenceSync::insert();
        m_releasePending = false;
    }
}

} // namespace rv::gl
