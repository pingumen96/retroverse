#pragma once

// StaticInstanceGroup (ADR-0011 §3, I3 step 8): the instance records of a scene::InstanceClusters layout uploaded once
// into a buffer of their own and drawn as cluster ranges with glDrawElementsInstancedBaseInstance in every view of
// gl::SceneRenderer, the shadow map's and the depth pre-pass's included.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/vertex_array.hpp>
#include <rv/scene/instance_clusters.hpp>

namespace rv::gl {

class GpuMesh;
class MaterialRegistry;
class MeshRegistry;

/// A group of instances that never move (the collection's cartridges), planned once (scene::InstanceClusters: batches
/// pre-sorted per media family part and texture array chunk, instances ordered by cluster) and uploaded once into an
/// immutable buffer of its own (GL_DYNAMIC_STORAGE_BIT): a still room writes no instance bytes. Each frame the renderer
/// culls the clusters per view (RenderView::staticGroups) and draws ranges; setAttributes() rewrites one record's 16
/// bytes of attributes (a layer that arrived, the highlight, the hidden flag of an item lifted out) and nothing else.
/// Every mesh of the layout gets a vertex array of the group's that reads the mesh's vertices and the group's records.
///
/// The meshes and materials are the registries' the renderer draws with: they must outlive the group, and
/// MeshRegistry::clear() (new content) requires the group to be destroyed first. Move-only; a default-constructed group
/// is empty and draws nothing. Main thread only; destroy it while the GL context is current.
class StaticInstanceGroup {
public:
    StaticInstanceGroup() = default;

    /// Uploads `layout`'s records and creates a vertex array for each of its meshes, labeled `debugName`. Errors:
    /// InvalidArgument for a mesh the registry does not hold, and the allocation errors of Buffer and VertexArray.
    [[nodiscard]] static Expected<StaticInstanceGroup> create(scene::InstanceClusters layout, const MeshRegistry& meshes,
                                                              std::string_view debugName);
    /// Builds the layout of `instances` (scene::InstanceClusters::build, the meshes' bounds and the materials' passes
    /// from the registries) and uploads it. Errors: those of the build and of create().
    [[nodiscard]] static Expected<StaticInstanceGroup> create(std::span<const scene::StaticInstance> instances,
                                                              std::uint32_t clusterCount, const MeshRegistry& meshes,
                                                              const MaterialRegistry& materials,
                                                              std::string_view debugName);

    [[nodiscard]] const scene::InstanceClusters& layout() const noexcept { return m_layout; }
    [[nodiscard]] bool valid() const noexcept { return m_buffer.valid(); }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

    /// Replaces the attributes of source instance `sourceIndex` and writes them into its record (16 bytes, one
    /// glNamedBufferSubData) when they changed. Errors: OutOfRange beyond the instances, those of Buffer::update.
    [[nodiscard]] Status setAttributes(std::size_t sourceIndex, const scene::InstanceAttributes& attributes);

    /// Bytes written by create(): every record.
    [[nodiscard]] std::uint64_t uploadedBytes() const noexcept { return m_uploadedBytes; }
    /// Bytes written by setAttributes() since create().
    [[nodiscard]] std::uint64_t writtenBytes() const noexcept { return m_writtenBytes; }

    /// The mesh of batch `batch` (below layout().batches().size()), from the registry.
    [[nodiscard]] const GpuMesh& meshOf(std::uint32_t batch) const;
    /// Draws `draw` (a range of one batch's records) with the bound program and state.
    void draw(const scene::StaticDraw& draw) const;
    /// Draws the single record at `position` (the outline's mark).
    void drawRecord(std::uint32_t position) const;

private:
    scene::InstanceClusters m_layout;
    /// The registry of the layout's meshes (looked up at each draw: it may grow and move its meshes).
    const MeshRegistry* m_meshes = nullptr;
    Buffer m_buffer;
    /// Per batch: the group's vertex array of the batch's mesh (an index into m_vertexArrays).
    std::vector<std::size_t> m_batchArrays;
    std::vector<VertexArray> m_vertexArrays;
    std::uint64_t m_uploadedBytes = 0;
    std::uint64_t m_writtenBytes = 0;
    std::string m_debugName;
};

} // namespace rv::gl
