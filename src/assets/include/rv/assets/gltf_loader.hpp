#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include <rv/assets/model_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

struct GltfLoadOptions {
    /// Decode images. When false, ModelData::images stays empty and materials carry no texture references.
    bool loadImages = true;
    /// Generate tangents (geometry::computeTangents) for primitives without a TANGENT attribute.
    bool generateTangents = true;
    /// Keep "ColliderProxy*" nodes out of ModelData::drawItems (they are still reported as sockets).
    bool excludeColliderProxies = true;
    /// Upper bound for the model file and for each external buffer or image it references.
    std::uint64_t maxFileBytes = 256ull << 20;
};

/// Loads a glTF 2.0 model (.gltf with external or base64 buffers, or .glb) with fastgltf.
///
/// Conventions: glTF axes and units are the runtime ones (ADR-0002), so no axis conversion happens; UVs keep the
/// glTF top-left origin and images are never flipped.
/// - Scene: the default scene, else scene 0, else every root node.
/// - Nodes: TRS or matrix; world transforms through the hierarchy. Cycles, shared children and non-finite
///   transforms are errors.
/// - Geometry: triangle lists, strips and fans (converted to lists); points and lines are skipped with a warning.
///   Missing indices are generated; missing normals become flat normals (warning); TANGENT is used when present,
///   otherwise tangents are generated when `generateTangents`; TEXCOORD_0 (float or normalized integers), zeros
///   when missing. NaN/Inf vertex data, out-of-range indices and accessors outside their buffers are errors.
/// - Extensions: KHR_texture_transform (offset/rotation/scale baked into TEXCOORD_0) and
///   KHR_materials_emissive_strength are supported, KHR_materials_unlit is honored when present; any other entry
///   of "extensionsRequired" is an error naming the extension.
/// - Images: from buffer views, data URIs or files relative to the glTF (no "..", no absolute paths), decoded with
///   decodeImage(); failures become warnings and the affected textures are dropped.
/// - Materials: glTF metallic-roughness factors (linear), alpha mode, cutoff, double-sided, texture references
///   through glTF textures to ModelData::images.
/// - Sockets (style §9.2, node names in the active scene): "Anchor_<Id>" -> Anchor with id <Id>; "ScreenSurface"
///   or "ScreenSurface_<n>" -> ScreenSurface (a mesh is expected, warning otherwise); "ColliderProxy*" ->
///   ColliderProxy; "PowerButton" and "Speaker*" -> Other; the id is the node name except for anchors.
///   Socket::transform = node world transform, Socket::bounds = node mesh bounds in node space,
///   Socket::meshIndex = first primitive of the node mesh.
/// - Diagnostics (warnings): non-uniform or mirroring scale on a node with a mesh, model bounds larger than 20 m or
///   smaller than 5 mm (probably exported in centimeters or millimeters).
///
/// Errors: NotFound / IoError (file), OutOfRange (size limits), ParseError (invalid glTF, GLB container or data),
/// Unsupported (required extension, sparse accessors, unsupported attribute formats).
[[nodiscard]] Expected<ModelData> loadGltf(const std::filesystem::path& path, const GltfLoadOptions& options = {});

/// Same as loadGltf() for a file already in memory. External buffers and images resolve against
/// `baseDirectory`; an empty `baseDirectory` makes external references unavailable.
[[nodiscard]] Expected<ModelData> loadGltfFromMemory(std::span<const std::byte> bytes,
                                                     const std::filesystem::path& baseDirectory,
                                                     const GltfLoadOptions& options = {});

} // namespace rv::assets
