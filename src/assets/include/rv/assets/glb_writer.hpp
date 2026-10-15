#pragma once

// glTF 2.0 / GLB writer (GPU-free): the minimal valid files that describe the given nodes, meshes and materials. It
// writes the CRT-like fixture of the asset preview tool (retroverse-asset-preview --write-fixture) and the models of
// the loader tests, which inject deliberate errors through GlbDescription::patchJson.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_data.hpp>

namespace rv::assets {

/// glTF node: TRS (translation, rotation, scale) unless `matrix` is set.
struct GlbNode {
    std::string name;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< glm order (w, x, y, z)
    glm::vec3 scale{1.0f};
    std::optional<glm::mat4> matrix; ///< written as "matrix" (column-major) instead of TRS
    std::vector<int> children;
    std::optional<int> mesh;
};

/// glTF mesh with a single primitive. Empty optional attributes are not written.
struct GlbMesh {
    std::string name;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
    std::vector<glm::vec2> uvs;
    std::vector<std::uint32_t> indices; ///< empty: non-indexed primitive
    std::optional<int> material;
    int mode = 4; ///< glTF primitive mode, 4 = TRIANGLES
};

/// Metallic-roughness material with an optional embedded base color image.
struct GlbMaterial {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.5f;
    /// Embedded as PNG (encodePng) in a buffer view and used as baseColorTexture.
    std::optional<ImageData> baseColorImage;
    /// Raw image bytes embedded instead of `baseColorImage` when not empty (corrupt-data tests).
    std::vector<std::byte> baseColorImageBytes;
};

struct GlbDescription {
    std::vector<GlbNode> nodes;
    /// Roots of scene 0 (the default scene); empty means every node that is no other node's child.
    std::vector<int> sceneRoots;
    std::vector<GlbMesh> meshes;
    std::vector<GlbMaterial> materials;
    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;
    /// Called on the finished glTF JSON before serialization (add extensions, break things on purpose).
    std::function<void(nlohmann::json&)> patchJson;
};

/// glTF JSON (buffer 0 has no "uri") and the binary payload it references.
struct GltfParts {
    nlohmann::json json;
    std::vector<std::byte> bin;
};

/// The glTF JSON of `description` and its binary payload: one 4-byte aligned buffer view per attribute, index list and
/// embedded image, accessors with the bounds of their positions (non-finite components are left out of them: JSON
/// cannot hold them). Errors: InvalidArgument when a base color image cannot be encoded as PNG (an invalid ImageData).
[[nodiscard]] Expected<GltfParts> buildGltfParts(const GlbDescription& description);

/// Binary glTF: 12-byte header, JSON chunk (space padded), BIN chunk (zero padded; none without a payload), 4-byte
/// aligned. Errors: those of buildGltfParts, ParseError when the JSON cannot be serialized (a string that is not
/// UTF-8), OutOfRange for a container beyond the 4 GiB a GLB header can describe.
[[nodiscard]] Expected<std::vector<std::byte>> buildGlb(const GlbDescription& description);

/// .gltf text with buffer 0 embedded as a base64 data URI. Errors: as buildGlb.
[[nodiscard]] Expected<std::string> buildGltfWithEmbeddedBuffer(const GlbDescription& description);

/// .gltf text whose buffer 0 references `binUri`, plus the payload the caller writes next to it.
struct GltfFiles {
    std::string json;
    std::vector<std::byte> bin;
};
/// Errors: as buildGlb.
[[nodiscard]] Expected<GltfFiles> buildGltfWithExternalBuffer(const GlbDescription& description,
                                                              std::string_view binUri);

/// Writes buildGlb(description) to `path` atomically (temporary file and rename, parent directories created).
/// Errors: those of buildGlb and core::writeFileAtomic.
[[nodiscard]] Status writeGlb(const std::filesystem::path& path, const GlbDescription& description);

/// Unit quad in the XY plane facing +Z (CCW from the front), uv (0, 0) at the top-left; `withNormals` adds +Z
/// normals.
[[nodiscard]] GlbMesh makeQuadMesh(std::string name, float width = 1.0f, float height = 1.0f, bool withNormals = true);

/// The triangles of `mesh` as a GLB mesh named after it: positions, normals, tangents, uvs and indices, drawn with
/// `material`.
[[nodiscard]] GlbMesh toGlbMesh(const geometry::MeshData& mesh, std::optional<int> material = std::nullopt);

} // namespace rv::assets
