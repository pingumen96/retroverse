#include <rv/render/static_instance_group.hpp>

#include <cstddef>
#include <map>
#include <utility>

#include <fmt/format.h>
#include <glad/gl.h>

#include <rv/render/gpu_mesh.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>

namespace rv::gl {

Expected<StaticInstanceGroup> StaticInstanceGroup::create(scene::InstanceClusters layout, const MeshRegistry& meshes,
                                                          std::string_view debugName) {
    StaticInstanceGroup group;
    group.m_meshes = &meshes;
    group.m_debugName = std::string(debugName);
    const std::span<const scene::InstanceData> records = layout.instances();
    if (!records.empty()) {
        auto buffer = Buffer::create(std::as_bytes(records), GL_DYNAMIC_STORAGE_BIT, group.m_debugName + ".instances");
        if (!buffer) {
            return propagate(buffer.error());
        }
        group.m_buffer = std::move(*buffer);
        group.m_uploadedBytes = records.size_bytes();
    }
    // One vertex array per mesh, shared by the batches of that mesh (a mesh drawn with several materials).
    std::map<std::uint32_t, std::size_t> arrayOfMesh;
    for (const scene::StaticBatch& batch : layout.batches()) {
        if (!meshes.contains(batch.mesh)) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("mesh {} is not registered", batch.mesh.value),
                             group.m_debugName);
        }
        const GpuMesh& mesh = meshes.mesh(batch.mesh);
        auto [found, added] = arrayOfMesh.try_emplace(batch.mesh.value, group.m_vertexArrays.size());
        if (added) {
            auto vertexArray = mesh.createVertexArray(group.m_buffer, fmt::format("{}.{}", group.m_debugName,
                                                                                  mesh.debugName()));
            if (!vertexArray) {
                return propagate(vertexArray.error());
            }
            group.m_vertexArrays.push_back(std::move(*vertexArray));
        }
        group.m_batchArrays.push_back(found->second);
    }
    group.m_layout = std::move(layout);
    return group;
}

Expected<StaticInstanceGroup> StaticInstanceGroup::create(std::span<const scene::StaticInstance> instances,
                                                          std::uint32_t clusterCount, const MeshRegistry& meshes,
                                                          const MaterialRegistry& materials,
                                                          std::string_view debugName) {
    scene::StaticGroupOptions options;
    options.localBounds = [&meshes](scene::MeshHandle mesh) {
        return meshes.bounds(mesh);
    };
    options.passOf = [&materials](scene::MaterialHandle material) {
        return materials.pass(material);
    };
    auto layout = scene::InstanceClusters::build(instances, clusterCount, options);
    if (!layout) {
        Error error = layout.error();
        error.context = std::string(debugName);
        return propagate(std::move(error));
    }
    return create(std::move(*layout), meshes, debugName);
}

Status StaticInstanceGroup::setAttributes(std::size_t sourceIndex, const scene::InstanceAttributes& attributes) {
    if (sourceIndex >= m_layout.instances().size()) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("instance {} of {}", sourceIndex, m_layout.instances().size()), m_debugName);
    }
    if (!m_layout.setAttributes(sourceIndex, attributes)) {
        return ok();
    }
    const std::uint32_t position = m_layout.positionOf(sourceIndex);
    const std::size_t offset = position * sizeof(scene::InstanceData) + offsetof(scene::InstanceData, attributes);
    if (Status written = m_buffer.update(offset, std::as_bytes(std::span(&attributes, 1))); !written) {
        return written;
    }
    m_writtenBytes += sizeof(scene::InstanceAttributes);
    return ok();
}

const GpuMesh& StaticInstanceGroup::meshOf(std::uint32_t batch) const {
    return m_meshes->mesh(m_layout.batches()[batch].mesh);
}

void StaticInstanceGroup::draw(const scene::StaticDraw& draw) const {
    if (draw.batch >= m_batchArrays.size()) {
        return;
    }
    meshOf(draw.batch).drawInstanced(m_vertexArrays[m_batchArrays[draw.batch]], draw.instanceCount,
                                             draw.firstInstance);
}

void StaticInstanceGroup::drawRecord(std::uint32_t position) const {
    if (position >= m_layout.instances().size()) {
        return;
    }
    draw(scene::StaticDraw{m_layout.batchOf(position), position, 1});
}

} // namespace rv::gl
