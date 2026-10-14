#pragma once

// Internal part of the glTF loader: bounds-checked accessor reading and primitive conversion. Not public API.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fastgltf/types.hpp>
#include <glm/glm.hpp>

#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_data.hpp>

namespace rv::assets::detail {

/// KHR_texture_transform, baked into TEXCOORD_0: uv' = offset + R(rotation) * (scale * uv), with
/// R(r) * (u, v) = (cos r * u + sin r * v, -sin r * u + cos r * v) (the convention of the Khronos sample viewer).
struct UvTransform {
    glm::vec2 offset{0.0f};
    float rotation = 0.0f;
    glm::vec2 scale{1.0f};

    [[nodiscard]] bool isIdentity() const noexcept;
    [[nodiscard]] glm::vec2 apply(const glm::vec2& uv) const noexcept;
    bool operator==(const UvTransform&) const = default;
};

/// Resolved glTF buffers (one byte span per glTF buffer, exactly byteLength long).
using BufferSpans = std::span<const std::span<const std::byte>>;

/// Bytes of a buffer view, bounds-checked against `buffers`.
[[nodiscard]] Expected<std::span<const std::byte>> bufferViewBytes(const fastgltf::Asset& asset, BufferSpans buffers,
                                                                   std::size_t viewIndex, const std::string& what);

struct PrimitiveConversion {
    bool generateTangents = true;
    /// Transform of the primitive's material to bake into TEXCOORD_0, or null.
    const UvTransform* uvTransform = nullptr;
};

/// Vertices and indices the rest of the model may still produce. Accessors can be shared by any number of
/// primitives and flat normals turn every index into a vertex, so per-accessor limits alone would let a small file
/// expand into gigabytes.
struct GeometryBudget {
    std::size_t vertices = 0;
    std::size_t indices = 0;
};

/// Converts one glTF primitive into an indexed triangle list that passes geometry::validateMesh. Returns an empty
/// optional (after appending a warning) when the primitive is skipped: points or lines, or no triangle at all.
/// Every POSITION vertex read and every vertex and index produced is taken from `budget` (the output before it is
/// allocated); exceeding the budget is an OutOfRange error.
[[nodiscard]] Expected<std::optional<geometry::MeshData>>
convertPrimitive(const fastgltf::Asset& asset, BufferSpans buffers, const fastgltf::Primitive& primitive,
                 const PrimitiveConversion& conversion, GeometryBudget& budget, const std::string& label,
                 std::vector<std::string>& warnings);

} // namespace rv::assets::detail
