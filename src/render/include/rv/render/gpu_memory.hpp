#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/vec2.hpp>

namespace rv::gl {

// The renderer's own accounting of the GPU memory it allocates (I3 step 0d; ADR-0011 §9: "memory by the renderer's own
// accounting ... not by the driver"): every texture, render target and buffer of rv_render records its bytes, computed
// from its format, size, levels and layers, under a kind, in the process's ledger (gpuMemoryLedger()) for as long as
// it lives. The run report's gpuMemory and the overlay's Stats panel read the ledger. What the vendored backends of Dear
// ImGui and RmlUi allocate (their fonts and images) is not the renderer's and is not counted, nor is what the driver
// adds (alignment, compression metadata, the window's own buffers).

/// What an allocation is for.
enum class GpuMemoryKind : std::uint8_t {
    /// The maps of the procedural surfaces (gl::TextureCache::procedural).
    ProceduralSurfaces,
    /// The collection's texture arrays, fronts and spines (I3 step 6).
    CollectionArrays,
    /// The hero textures of the focused, pulled-out or inspected items (I3 step 12).
    HeroTextures,
    /// Video pictures and their pixel buffer rings (gl::VideoTexture).
    Video,
    /// Render targets: the views' framebuffers (HDR scene, depth, ambient occlusion, bloom, post-processing, the
    /// output), the shadow map, the self-checks' canvases.
    Targets,
    /// Every other texture: the scene's files, patterns and model images, the default textures.
    SceneTextures,
    /// Buffer storage: vertices and indices, instances, uniform blocks, debug lines.
    Buffers,
};
inline constexpr std::size_t kGpuMemoryKindCount = 7;

/// Every kind, in declaration order.
inline constexpr std::array<GpuMemoryKind, kGpuMemoryKindCount> kGpuMemoryKinds{
    GpuMemoryKind::ProceduralSurfaces, GpuMemoryKind::CollectionArrays, GpuMemoryKind::HeroTextures,
    GpuMemoryKind::Video,              GpuMemoryKind::Targets,          GpuMemoryKind::SceneTextures,
    GpuMemoryKind::Buffers,
};

/// The run report's key: "proceduralSurfaces", "collectionArrays", "heroTextures", "video", "targets",
/// "sceneTextures", "buffers".
[[nodiscard]] std::string_view toString(GpuMemoryKind kind) noexcept;
/// The kind toString() names, or nothing.
[[nodiscard]] std::optional<GpuMemoryKind> gpuMemoryKindFromString(std::string_view name) noexcept;
/// A short label for the Stats panel's narrow column (toString() in its tooltip): "Surfaces", "Arrays", "Hero",
/// "Video", "Targets", "Textures", "Buffers".
[[nodiscard]] std::string_view displayName(GpuMemoryKind kind) noexcept;

/// Storage formats of the renderer's textures and render targets, by what a texel or a block takes.
enum class GpuFormat : std::uint8_t {
    Rgba8,           ///< GL_RGBA8, 4 bytes
    Srgb8Alpha8,     ///< GL_SRGB8_ALPHA8, 4 bytes
    Rgba16F,         ///< GL_RGBA16F, 8 bytes
    R8,              ///< GL_R8, 1 byte
    Rg8,             ///< GL_RG8, 2 bytes
    Rg16F,           ///< GL_RG16F, 4 bytes
    R11fG11fB10f,    ///< GL_R11F_G11F_B10F, 4 bytes: three unsigned small floats, no alpha
    Depth24Stencil8, ///< GL_DEPTH24_STENCIL8, 4 bytes
    Depth32F,        ///< GL_DEPTH_COMPONENT32F, 4 bytes
    /// GL_COMPRESSED_RGB_S3TC_DXT1_EXT / GL_COMPRESSED_SRGB_S3TC_DXT1_EXT and their alpha variants: blocks of 4 x 4
    /// texels in 8 bytes, every level a whole number of blocks.
    Bc1,
};

/// Bytes of one level of `size` (each side at least 1; BC1 rounds each side up to whole blocks); 0 for an empty size.
[[nodiscard]] std::size_t textureLevelBytes(GpuFormat format, glm::ivec2 size) noexcept;
/// Bytes of the first `levels` levels of a texture of `size` with `layers` layers (an array texture; 1 otherwise):
/// each level halves both sides, down to 1. 0 for an empty size, no level or no layer.
[[nodiscard]] std::size_t textureBytes(GpuFormat format, glm::ivec2 size, int levels, int layers = 1) noexcept;

/// Bytes of one kind: allocated now, the most at any moment so far, and the allocations alive now.
struct GpuMemoryUsage {
    std::uint64_t bytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t allocations = 0;

    bool operator==(const GpuMemoryUsage&) const = default;
};

/// The bytes allocated per kind, with their peaks. Trivially destructible, so that an allocation released during
/// static destruction never touches a destroyed ledger. Not thread-safe: GPU objects live on the main thread.
class GpuMemoryLedger {
public:
    /// Records an allocation of `bytes` (0 counts as an allocation of nothing).
    void add(GpuMemoryKind kind, std::uint64_t bytes) noexcept;
    /// Records the release of an allocation of `bytes` (never below 0).
    void remove(GpuMemoryKind kind, std::uint64_t bytes) noexcept;

    [[nodiscard]] const GpuMemoryUsage& usage(GpuMemoryKind kind) const noexcept;
    /// Every kind, in kGpuMemoryKinds order.
    [[nodiscard]] const std::array<GpuMemoryUsage, kGpuMemoryKindCount>& kinds() const noexcept { return m_kinds; }
    /// The bytes of every kind together, now and at their highest so far (the highest sum, not the sum of the peaks).
    [[nodiscard]] std::uint64_t totalBytes() const noexcept { return m_totalBytes; }
    [[nodiscard]] std::uint64_t peakTotalBytes() const noexcept { return m_peakTotalBytes; }

private:
    std::array<GpuMemoryUsage, kGpuMemoryKindCount> m_kinds{};
    std::uint64_t m_totalBytes = 0;
    std::uint64_t m_peakTotalBytes = 0;
};

/// The process's ledger, where every GPU object of rv_render records itself (main thread only).
[[nodiscard]] GpuMemoryLedger& gpuMemoryLedger() noexcept;

/// One allocation recorded in a ledger for its lifetime (RAII): the owner of a GPU object holds one beside it.
/// Move-only; a default-constructed or moved-from allocation records nothing.
class GpuMemoryAllocation {
public:
    GpuMemoryAllocation() noexcept = default;
    /// Records `bytes` of `kind` in `ledger`, which must outlive the allocation.
    GpuMemoryAllocation(GpuMemoryKind kind, std::uint64_t bytes, GpuMemoryLedger& ledger = gpuMemoryLedger()) noexcept;
    GpuMemoryAllocation(GpuMemoryAllocation&& other) noexcept;
    GpuMemoryAllocation& operator=(GpuMemoryAllocation&& other) noexcept;
    GpuMemoryAllocation(const GpuMemoryAllocation&) = delete;
    GpuMemoryAllocation& operator=(const GpuMemoryAllocation&) = delete;
    ~GpuMemoryAllocation();

    /// Releases the record (the allocation records nothing afterwards).
    void reset() noexcept;
    /// Moves the record to another kind (an object handed to another use); nothing for an empty allocation.
    void setKind(GpuMemoryKind kind) noexcept;

    [[nodiscard]] bool recorded() const noexcept { return m_ledger != nullptr; }
    [[nodiscard]] GpuMemoryKind kind() const noexcept { return m_kind; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return m_bytes; }

private:
    GpuMemoryLedger* m_ledger = nullptr;
    GpuMemoryKind m_kind = GpuMemoryKind::SceneTextures;
    std::uint64_t m_bytes = 0;
};

} // namespace rv::gl
