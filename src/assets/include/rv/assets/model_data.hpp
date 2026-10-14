#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/assets/material_desc.hpp>
#include <rv/core/aabb.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/socket.hpp>

namespace rv::assets {

/// One glTF node. Transforms are in meters (glTF axes = runtime axes, ADR-0002: +Y up, +Z front).
struct ModelNode {
    /// glTF node name, or "node_<index>" when the node is unnamed.
    std::string name;
    /// Index of the parent in ModelData::nodes, -1 for root nodes.
    int parent = -1;
    /// Transform relative to the parent (TRS or matrix from the file).
    glm::mat4 local{1.0f};
    /// Transform relative to the model root (product of the local transforms along the hierarchy).
    glm::mat4 world{1.0f};
    /// First entry of ModelData::meshes built from this node's glTF mesh; empty when the node has no mesh or none
    /// of its primitives could be loaded. The node's primitives are meshes[*mesh, *mesh + meshCount).
    std::optional<std::uint32_t> mesh;
    /// Number of consecutive ModelData::meshes entries that belong to this node's glTF mesh.
    std::uint32_t meshCount = 0;
};

/// One primitive to draw: ModelData::meshes[meshIndex] with `world` placement.
struct ModelDrawItem {
    std::uint32_t meshIndex = 0;
    /// Index into ModelData::materials; empty means the glTF default material.
    std::optional<std::uint32_t> material;
    /// Index into ModelData::nodes of the node that instances the primitive.
    std::uint32_t node = 0;
    /// Copy of nodes[node].world.
    glm::mat4 world{1.0f};
};

/// Summary for diagnostics and the asset-preview tool.
struct ModelStats {
    std::size_t meshes = 0;                  ///< glTF meshes in the file
    std::size_t primitives = 0;              ///< loaded triangle primitives (= ModelData::meshes.size())
    std::size_t triangles = 0;               ///< over ModelData::meshes (unique geometry, not multiplied by instances)
    std::size_t vertices = 0;                ///< over ModelData::meshes
    std::size_t materials = 0;               ///< = ModelData::materials.size()
    std::size_t images = 0;                  ///< decoded images (= ModelData::images.size())
    std::size_t nodes = 0;                   ///< = ModelData::nodes.size()
    std::vector<std::string> extensionsUsed; ///< glTF "extensionsUsed", in file order
};

/// CPU-side model loaded from glTF 2.0 / GLB (see gltf_loader.hpp).
struct ModelData {
    /// One mesh per loaded glTF triangle primitive, in file order (mesh by mesh, primitive by primitive), in the
    /// primitive's own (node-local) space. Every mesh passes geometry::validateMesh. MeshData::name is the glTF
    /// mesh name ("mesh_<index>" when unnamed), suffixed with "/<primitive>" for multi-primitive meshes.
    std::vector<geometry::MeshData> meshes;
    /// One material per glTF material, same order (glTF material index = index here). Factors are linear;
    /// unnamed materials are called "material_<index>".
    std::vector<MaterialDesc> materials;
    /// Successfully decoded images; TextureRef::imageIndex points here. Images that failed to decode are
    /// dropped (with a warning) and textures that used them are left empty.
    std::vector<ImageData> images;
    /// Every glTF node, same order as the file (glTF node index = index here).
    std::vector<ModelNode> nodes;
    /// Primitives instanced by the nodes of the active scene, in depth-first scene order.
    std::vector<ModelDrawItem> drawItems;
    /// Semantic sockets found in the active scene by node name (style §9.2), in depth-first scene order.
    std::vector<geometry::Socket> sockets;
    /// World-space (model root) bounds of the drawn primitives, from their transformed vertices.
    core::Aabb bounds;
    ModelStats stats;
    /// Non-fatal problems (missing normals, dropped textures, suspicious scale, ...), one sentence each.
    std::vector<std::string> warnings;
};

} // namespace rv::assets
