#include <rv/render/mesh_registry.hpp>

#include <limits>
#include <string>
#include <utility>

#include <rv/render/instance_buffer.hpp>

namespace rv::gl {

Expected<scene::MeshHandle> MeshRegistry::add(std::string_view key, const geometry::MeshData& mesh) {
    if (const std::optional<scene::MeshHandle> known = find(key)) {
        return *known;
    }
    if (m_entries.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return makeError(ErrorCode::OutOfRange, "too many meshes", std::string(key));
    }
    auto uploaded = GpuMesh::create(mesh, key);
    if (!uploaded) {
        return propagate(uploaded.error());
    }
    const auto index = static_cast<std::uint32_t>(m_entries.size());
    m_entries.push_back(Entry{std::move(*uploaded), 0});
    m_gpuBytes += m_entries.back().mesh.gpuBytes();
    m_keys.emplace(std::string(key), index);
    return scene::MeshHandle{index};
}

std::optional<scene::MeshHandle> MeshRegistry::find(std::string_view key) const {
    if (const auto found = m_keys.find(key); found != m_keys.end()) {
        return scene::MeshHandle{found->second};
    }
    return std::nullopt;
}

void MeshRegistry::clear() noexcept {
    m_entries.clear();
    m_keys.clear();
    m_gpuBytes = 0;
}

core::Aabb MeshRegistry::bounds(scene::MeshHandle handle) const noexcept {
    return contains(handle) ? m_entries[handle.value].mesh.bounds() : core::Aabb{};
}

Status MeshRegistry::attachInstanceBuffer(const InstanceBuffer& instances) {
    const std::uint64_t generation = instances.generation();
    for (Entry& entry : m_entries) {
        if (entry.instanceGeneration == generation) {
            continue;
        }
        if (auto attached = entry.mesh.attachInstanceBuffer(instances.buffer()); !attached) {
            return attached;
        }
        entry.instanceGeneration = generation;
    }
    return ok();
}

} // namespace rv::gl
