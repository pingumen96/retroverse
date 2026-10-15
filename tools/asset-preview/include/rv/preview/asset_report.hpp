#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/preview/preview_subject.hpp>

namespace rv::preview {

/// An image of a model, as the preview uploads it.
struct TextureReport {
    /// Index into assets::ModelData::images.
    std::uint32_t image = 0;
    glm::ivec2 size{0};
    int channels = 0;
    /// The material slots that use it: "base color of Body, emissive of Lamp".
    std::string usage;
    /// GPU memory: an RGBA8 texture with a full mip chain (gl::mipChainBytes), once per color space it is used in (sRGB
    /// for base color and emissive, linear for the data maps).
    std::size_t gpuBytes = 0;
};

/// The statistics of the subject the Statistics panel shows.
struct AssetReport {
    std::size_t nodes = 0;
    /// glTF meshes, or the parts of a prefab.
    std::size_t meshes = 0;
    /// Primitives drawn: the draw calls the asset costs at most per view (one per primitive instance; instancing and
    /// batching of equal meshes and materials can merge them).
    std::size_t primitives = 0;
    /// Triangles and vertices drawn per view: every primitive instance counted.
    std::size_t triangles = 0;
    std::size_t vertices = 0;
    /// Triangles of every mesh, each once (a mesh instanced by several nodes counted once; meshes that are not drawn,
    /// such as collision proxies, counted too).
    std::size_t uniqueTriangles = 0;
    /// glTF materials, or the material roles of a prefab.
    std::size_t materials = 0;
    std::size_t sockets = 0;
    /// The images of a model; none for a prefab, whose textures are procedural.
    std::vector<TextureReport> textures;
    /// Sum of TextureReport::gpuBytes.
    std::size_t textureBytes = 0;
    /// glTF "extensionsUsed", in file order.
    std::vector<std::string> extensions;
    /// Size of the bounds, meters; zero for the placeholder.
    glm::vec3 size{0.0f};
};

/// The statistics of `subject` (nothing but zeros for an empty preview).
[[nodiscard]] AssetReport buildAssetReport(const PreviewSubject& subject);

} // namespace rv::preview
