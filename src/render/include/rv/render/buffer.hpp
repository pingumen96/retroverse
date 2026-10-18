#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <glad/gl.h>

#include <rv/core/expected.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

/// Move-only RAII GPU buffer with immutable storage (glCreateBuffers + glNamedBufferStorage). Its size is fixed at
/// creation; contents can change through update() only when created with GL_DYNAMIC_STORAGE_BIT. A default-constructed
/// or moved-from Buffer is empty (id 0). Its storage is recorded in gpuMemoryLedger() under the kind it was created with
/// (GpuMemoryKind::Buffers unless the owner says otherwise). Requires a current GL 4.5 context; main thread only.
class Buffer {
public:
    Buffer() noexcept = default;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    /// Allocates `sizeBytes` (> 0) of storage with undefined contents. `storageFlags` are glNamedBufferStorage flags
    /// (0 for static data, GL_DYNAMIC_STORAGE_BIT to allow update(), mapping bits...). `debugName` labels the object
    /// for debuggers and error messages; `memoryKind` is what its storage is recorded as.
    [[nodiscard]] static Expected<Buffer> create(std::size_t sizeBytes, GLbitfield storageFlags,
                                                 std::string_view debugName,
                                                 GpuMemoryKind memoryKind = GpuMemoryKind::Buffers);
    /// Allocates storage sized and initialized from `initialData` (non-empty), e.g. std::as_bytes(std::span(vertices)).
    [[nodiscard]] static Expected<Buffer> create(std::span<const std::byte> initialData, GLbitfield storageFlags,
                                                 std::string_view debugName,
                                                 GpuMemoryKind memoryKind = GpuMemoryKind::Buffers);

    /// Copies `bytes` to `offsetBytes` (glNamedBufferSubData). Fails, without touching GL, unless the buffer was
    /// created with GL_DYNAMIC_STORAGE_BIT and the range lies inside it. An empty update is a no-op.
    [[nodiscard]] Status update(std::size_t offsetBytes, std::span<const std::byte> bytes);

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] bool valid() const noexcept { return m_id != 0; }
    /// Storage size in bytes.
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] GLbitfield storageFlags() const noexcept { return m_storageFlags; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    Buffer(GLuint id, std::size_t size, GLbitfield storageFlags, std::string debugName) noexcept;
    [[nodiscard]] static Expected<Buffer> allocate(std::size_t sizeBytes, const void* data, GLbitfield storageFlags,
                                                   std::string_view debugName, GpuMemoryKind memoryKind);
    void release() noexcept;

    GLuint m_id = 0;
    std::size_t m_size = 0;
    GLbitfield m_storageFlags = 0;
    std::string m_debugName;
    GpuMemoryAllocation m_memory;
};

} // namespace rv::gl
