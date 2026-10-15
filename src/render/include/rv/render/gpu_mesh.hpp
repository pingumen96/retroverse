#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <glad/gl.h>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {

/// Move-only GPU copy of a geometry::MeshData, drawn as an indexed triangle list: an immutable vertex buffer in the
/// standard 48-byte layout (geometry::VertexLayout::standard(): location 0 position, 1 normal, 2 tangent, 3 uv0, all
/// read from binding 0), an immutable uint32 index buffer and the vertex array that ties them together (DSA).
///
/// Instancing: attribute locations 4..7 hold the per-instance model matrix (local -> world), one column per location,
/// read by every mesh shader (pbr/pbr.vert). Attach a buffer of column-major glm::mat4 with attachInstanceBuffer()
/// (usually the renderer's InstanceBuffer) and draw with drawInstanced(). A default-constructed or moved-from GpuMesh
/// is empty. Main thread only.
class GpuMesh {
public:
    static constexpr GLuint kVertexBinding = 0;
    static constexpr GLuint kInstanceBinding = 1;
    /// First of the four attribute locations of the instance matrix: column i is read at location 4 + i.
    static constexpr GLuint kInstanceMatrixLocation = 4;
    /// Bytes between two instance matrices (one column-major glm::mat4).
    static constexpr std::size_t kInstanceStride = 64;

    GpuMesh() noexcept = default;

    /// Validates `mesh` (geometry::validateMesh) and uploads it. `debugName` labels the GL objects (the mesh name when
    /// empty). Errors: InvalidArgument for an empty mesh, ValidationFailed for an invalid one, OutOfRange for more
    /// indices than GLsizei holds, and the allocation errors of Buffer and VertexArray.
    [[nodiscard]] static Expected<GpuMesh> create(const geometry::MeshData& mesh, std::string_view debugName = {});

    /// Reads one model matrix per instance from `instances`, starting at `offsetBytes` (binding 1, divisor 1, 64-byte
    /// stride). The mesh does not own the buffer: keep it alive while drawing. Attaching again replaces the buffer.
    [[nodiscard]] Status attachInstanceBuffer(const Buffer& instances, std::size_t offsetBytes = 0);

    /// Draws `instanceCount` copies (glDrawElementsInstancedBaseInstance): instance i reads matrix `baseInstance + i`
    /// of the attached instance buffer. Binds the mesh's vertex array; the program and the render state are the
    /// caller's.
    void drawInstanced(std::uint32_t instanceCount, std::uint32_t baseInstance = 0) const;

    [[nodiscard]] bool valid() const noexcept { return m_vertexArray.valid(); }
    /// Bounds of the vertex positions, in mesh space.
    [[nodiscard]] const core::Aabb& bounds() const noexcept { return m_bounds; }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return m_indexCount / 3; }
    [[nodiscard]] std::size_t vertexCount() const noexcept { return m_vertexCount; }
    /// Storage of the vertex and index buffers, in bytes.
    [[nodiscard]] std::size_t gpuBytes() const noexcept { return m_vertices.size() + m_indices.size(); }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    GpuMesh(Buffer vertices, Buffer indices, VertexArray vertexArray, std::size_t vertexCount, std::size_t indexCount,
            const core::Aabb& bounds, std::string debugName) noexcept;

    Buffer m_vertices;
    Buffer m_indices;
    VertexArray m_vertexArray;
    std::size_t m_vertexCount = 0;
    std::size_t m_indexCount = 0;
    core::Aabb m_bounds;
    std::string m_debugName;
};

} // namespace rv::gl
