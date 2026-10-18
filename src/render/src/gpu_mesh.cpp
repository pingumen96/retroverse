#include <rv/render/gpu_mesh.hpp>

#include <cstddef>
#include <limits>
#include <span>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>

namespace rv::gl {

GpuMesh::GpuMesh(Buffer vertices, Buffer indices, VertexArray vertexArray, std::size_t vertexCount,
                 std::size_t indexCount, const core::Aabb& bounds, std::string debugName) noexcept
    : m_vertices(std::move(vertices)), m_indices(std::move(indices)), m_vertexArray(std::move(vertexArray)),
      m_vertexCount(vertexCount), m_indexCount(indexCount), m_bounds(bounds), m_debugName(std::move(debugName)) {}

Expected<GpuMesh> GpuMesh::create(const geometry::MeshData& mesh, std::string_view debugName) {
    std::string name(debugName.empty() ? std::string_view(mesh.name) : debugName);
    if (mesh.empty()) {
        return makeError(ErrorCode::InvalidArgument, "mesh has no triangles", name);
    }
    if (mesh.indices.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        return makeError(ErrorCode::OutOfRange, fmt::format("{} indices exceed GLsizei", mesh.indices.size()), name);
    }
    if (auto valid = geometry::validateMesh(mesh); !valid) {
        return propagate(valid.error());
    }

    auto vertices = Buffer::create(std::as_bytes(std::span(mesh.vertices)), 0, name + ".vertices");
    if (!vertices) {
        return propagate(vertices.error());
    }
    auto indices = Buffer::create(std::as_bytes(std::span(mesh.indices)), 0, name + ".indices");
    if (!indices) {
        return propagate(indices.error());
    }
    auto vertexArray = VertexArray::create(name);
    if (!vertexArray) {
        return propagate(vertexArray.error());
    }
    const geometry::VertexLayout& layout = geometry::VertexLayout::standard();
    Status status = vertexArray->setVertexBuffer(kVertexBinding, *vertices, 0, layout.stride);
    for (const geometry::VertexAttribute& attribute : layout.attributes) {
        if (status) {
            status = vertexArray->setFloatAttribute(attribute.location, kVertexBinding,
                                                    static_cast<int>(attribute.components), attribute.offset);
        }
    }
    if (status) {
        status = vertexArray->setElementBuffer(*indices);
    }
    if (!status) {
        return propagate(status.error());
    }

    core::Aabb bounds;
    for (const geometry::Vertex& vertex : mesh.vertices) {
        bounds.expand(vertex.position);
    }
    return GpuMesh(std::move(*vertices), std::move(*indices), std::move(*vertexArray), mesh.vertices.size(),
                   mesh.indices.size(), bounds, std::move(name));
}

Status GpuMesh::attachInstanceBuffer(const Buffer& instances, std::size_t offsetBytes) {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "cannot attach instances to an empty mesh", instances.debugName());
    }
    Status status = m_vertexArray.setVertexBuffer(kInstanceBinding, instances, offsetBytes, kInstanceStride);
    for (GLuint column = 0; column < 4 && status; ++column) {
        status = m_vertexArray.setFloatAttribute(kInstanceMatrixLocation + column, kInstanceBinding, 4,
                                                 offsetof(scene::InstanceData, model) + column * sizeof(glm::vec4));
    }
    if (status) {
        status = m_vertexArray.setUintAttribute(kInstanceAttributesLocation, kInstanceBinding, 4,
                                                offsetof(scene::InstanceData, attributes));
    }
    if (status) {
        status = m_vertexArray.setBindingDivisor(kInstanceBinding, 1);
    }
    return status;
}

void GpuMesh::drawInstanced(std::uint32_t instanceCount, std::uint32_t baseInstance) const {
    if (m_indexCount == 0 || instanceCount == 0) {
        return;
    }
    m_vertexArray.bind();
    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, static_cast<GLsizei>(m_indexCount), GL_UNSIGNED_INT, nullptr,
                                        static_cast<GLsizei>(instanceCount), baseInstance);
}

} // namespace rv::gl
