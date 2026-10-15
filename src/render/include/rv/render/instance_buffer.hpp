#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <glm/mat4x4.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/gl_object.hpp>

namespace rv::gl {

/// Per-instance model matrices of the instanced draws (GpuMesh attribute locations 4..7, divisor 1): one persistently
/// mapped, coherent buffer split into kRegionCount regions used in turn. Each write() fills the next region after
/// waiting for the fence placed by release() once the draws that last read it were issued, so the CPU never
/// overwrites matrices the GPU may still be reading and, with three regions, normally never waits at all.
///
/// When a write does not fit a region, the buffer is reallocated larger: a new GL buffer with a new generation()
/// (unique across every InstanceBuffer), which the meshes must be attached to again (MeshRegistry::
/// attachInstanceBuffer). Move-only; a default-constructed or moved-from buffer is empty. Main thread only.
class InstanceBuffer {
public:
    /// Regions used in turn: the frame being recorded, and up to two frames the GPU may still be drawing.
    static constexpr std::size_t kRegionCount = 3;
    /// Largest region, in matrices: base instances of the last region still fit 32 bits.
    static constexpr std::size_t kMaxRegionCapacity = std::size_t{1} << 24;
    /// Longest wait for the GPU to release a region before write() gives up.
    static constexpr std::uint64_t kFenceTimeoutNanoseconds = 1'000'000'000;

    InstanceBuffer() noexcept = default;
    InstanceBuffer(InstanceBuffer&& other) noexcept;
    InstanceBuffer& operator=(InstanceBuffer&& other) noexcept;
    InstanceBuffer(const InstanceBuffer&) = delete;
    InstanceBuffer& operator=(const InstanceBuffer&) = delete;
    ~InstanceBuffer() = default;

    /// Allocates kRegionCount regions of `regionCapacity` matrices (at least 1, at most kMaxRegionCapacity), labeled
    /// `debugName`. Errors: OutOfRange for a capacity above the maximum, GraphicsError when the storage cannot be
    /// allocated or mapped.
    [[nodiscard]] static Expected<InstanceBuffer> create(std::size_t regionCapacity, std::string_view debugName);

    /// Copies `transforms` into the next region and returns the index of the first one in the buffer: instanced draws
    /// add it to their base instance. A previous write that was not released is released first. Grows the buffer when
    /// the transforms do not fit (capacity doubled, or as many as needed). An empty write returns 0 and uses no region.
    /// Errors: InvalidArgument for an empty buffer, OutOfRange beyond kMaxRegionCapacity, GraphicsError when the GPU
    /// does not release the region within kFenceTimeoutNanoseconds or a larger buffer cannot be allocated.
    [[nodiscard]] Expected<std::uint32_t> write(std::span<const glm::mat4> transforms);
    /// Same for several lists of transforms written one after the other into one region (a view and its shadow map):
    /// part i starts at the returned index plus the sizes of the parts before it. Parts may be empty; when all are, it
    /// returns 0 and uses no region.
    [[nodiscard]] Expected<std::uint32_t> write(std::span<const std::span<const glm::mat4>> parts);

    /// Fences the region of the last write(): call it once the draws that read it are issued.
    void release();

    [[nodiscard]] const Buffer& buffer() const noexcept { return m_buffer; }
    [[nodiscard]] bool valid() const noexcept { return m_buffer.valid(); }
    /// Identity of the current GL buffer: changes when write() reallocates it; 0 for an empty buffer.
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    /// Matrices per region.
    [[nodiscard]] std::size_t regionCapacity() const noexcept { return m_regionCapacity; }

private:
    /// Replaces the GL buffer with a mapped one of kRegionCount regions of `regionCapacity` matrices.
    [[nodiscard]] Status allocate(std::size_t regionCapacity);

    Buffer m_buffer;
    /// Persistent, coherent mapping of the whole buffer; valid while m_buffer lives.
    std::byte* m_mapped = nullptr;
    std::size_t m_regionCapacity = 0;
    std::array<FenceSync, kRegionCount> m_fences;
    /// Region of the last write.
    std::size_t m_region = kRegionCount - 1;
    bool m_releasePending = false;
    std::uint64_t m_generation = 0;
    std::string m_debugName;
};

} // namespace rv::gl
