#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/render/gpu_mesh.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

class InstanceBuffer;

/// The meshes of the scene on the GPU, one GpuMesh each, behind the scene::MeshHandle of the render items. Meshes are
/// registered under keys that identify their geometry (scene::ScenePart::meshKey: a prefab or primitive kind with its
/// parameters and part role; "<model>#mesh<i>" for model primitives): equal keys share one upload, so the 24
/// cartridges of the test room draw one shell mesh and one label mesh. Handles are indices in registration order.
/// Main thread only; destroy it while the GL context is current.
class MeshRegistry {
public:
    /// The handle of `key`: registered before, or a new GpuMesh uploaded from `mesh` (labeled with the key). Equal keys
    /// must describe equal geometry; a second registration uploads nothing. Errors: those of GpuMesh::create (an
    /// empty or invalid mesh), and OutOfRange beyond 2^32 meshes; the key stays unregistered.
    [[nodiscard]] Expected<scene::MeshHandle> add(std::string_view key, const geometry::MeshData& mesh);

    /// The handle registered under `key`, if any.
    [[nodiscard]] std::optional<scene::MeshHandle> find(std::string_view key) const;

    /// Releases every mesh and forgets every key: the handles returned so far are no longer valid, and render items
    /// that use them must not be drawn any more. For new content in the same renderer (the asset preview loads another
    /// model).
    void clear() noexcept;

    /// True for a handle returned by add().
    [[nodiscard]] bool contains(scene::MeshHandle handle) const noexcept { return handle.value < m_entries.size(); }
    /// The mesh of a handle returned by add() (std::out_of_range otherwise).
    [[nodiscard]] const GpuMesh& mesh(scene::MeshHandle handle) const { return m_entries.at(handle.value).mesh; }
    /// Mesh-space bounds of a handle, for culling; an empty box for an unknown handle.
    [[nodiscard]] core::Aabb bounds(scene::MeshHandle handle) const noexcept;
    /// Registered meshes.
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    /// Vertex and index storage of every registered mesh, in bytes.
    [[nodiscard]] std::size_t gpuBytes() const noexcept { return m_gpuBytes; }

    /// Makes every mesh read its instance records (locations 4..8) from `instances`. Meshes already attached to
    /// its current generation are skipped, so calling it before every view costs a loop unless the instance buffer was
    /// reallocated or meshes were added since. Errors: those of GpuMesh::attachInstanceBuffer.
    [[nodiscard]] Status attachInstanceBuffer(const InstanceBuffer& instances);

private:
    struct Entry {
        GpuMesh mesh;
        /// InstanceBuffer::generation() the mesh reads its instances from; 0 before the first attachment.
        std::uint64_t instanceGeneration = 0;
    };

    std::vector<Entry> m_entries;
    std::map<std::string, std::uint32_t, std::less<>> m_keys;
    std::size_t m_gpuBytes = 0;
};

} // namespace rv::gl
