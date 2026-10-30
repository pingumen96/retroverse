#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/vec2.hpp>

#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

/// The passes that draw a view through the targets of its output (SceneRenderer::render, gl::ViewTargets), in the order
/// of a frame.
enum class ViewPass : std::uint8_t {
    /// The HDR target cleared, the depth pre-pass into its depth.
    DepthPrePass,
    /// The ambient occlusion estimated at half the resolution (or the occlusion cleared to "nothing occluded").
    AmbientOcclusion,
    /// Its depth-aware blur to the full resolution.
    AmbientOcclusionBlur,
    /// The lit pass into the HDR target.
    LitPass,
    /// The bloom chain, down and up.
    Bloom,
    /// The post pass: the tonemapped image.
    PostPass,
    /// The anti-aliasing pass into the output.
    AntiAliasing,
    /// The hover outline: its mark into the scene's stencil, its edge over the output.
    Outline,
    /// The debug lines over the output, depth-tested against the scene.
    DebugLines,
    /// The overlays over the output, its readback (screenshots) or its copy to the window.
    Present,
};

/// The images of the targets of one output (gl::ViewTargets).
enum class ViewImage : std::uint8_t {
    SceneDepth,          ///< hdr()'s depth/stencil texture
    Radiance,            ///< hdr()'s first color attachment (SceneOutput::Radiance)
    Emission,            ///< hdr()'s second color attachment (SceneOutput::Emission)
    AmbientOcclusionRaw, ///< ambientOcclusionRaw()
    AmbientOcclusion,    ///< ambientOcclusion()
    BloomLevel0,         ///< bloomLevels()[0], then [1] to [5]
    BloomLevel1,
    BloomLevel2,
    BloomLevel3,
    BloomLevel4,
    BloomLevel5,
    Tonemapped, ///< tonemapped()
    Output,     ///< output()'s color, which outputWithSceneDepth() draws into too
};
inline constexpr std::size_t kViewImageCount = 13;

/// What the size of an image follows.
enum class ViewImageScale : std::uint8_t {
    Full,       ///< the output's size
    Half,       ///< ssaoTargetSize(): half the output's, rounded up
    BloomLevel, ///< bloomLevelSizes()[bloomLevel]
};

/// One image of an output's targets: its format, its size, the passes of a frame it lives through and the storage it
/// takes. An image with `storageOf` allocates nothing: it is a texture view (glTextureView) of that image's storage, in
/// the same view class (texels of one size), which it takes over once that image's last pass is over; an image
/// `writtenInPlace` takes it over within that last pass, which then reads each texel of it only at the texel it writes
/// (texelFetch at gl_FragCoord, after glTextureBarrier). ViewTargets allocates its targets from this plan.
struct ViewImagePlan {
    ViewImage image = ViewImage::Output;
    /// Its label after the output's name ("<debugName>.<name>").
    std::string_view name;
    GpuFormat format = GpuFormat::Srgb8Alpha8;
    ViewImageScale scale = ViewImageScale::Full;
    /// With ViewImageScale::BloomLevel.
    std::size_t bloomLevel = 0;
    /// The first pass of a frame that writes it (a clear counts) and the last one that reads it.
    ViewPass firstPass = ViewPass::DepthPrePass;
    ViewPass lastPass = ViewPass::Present;
    /// The image whose storage it takes over, one with storage of its own; none: it has its own.
    std::optional<ViewImage> storageOf;
    bool writtenInPlace = false;
};

/// Every image, in ViewImage order.
[[nodiscard]] std::span<const ViewImagePlan, kViewImageCount> viewTargetPlan() noexcept;
/// The plan of `image`.
[[nodiscard]] const ViewImagePlan& viewImagePlan(ViewImage image) noexcept;
/// The size of `plan`'s image for an output of `viewSize` pixels (sides below 1 count as 1).
[[nodiscard]] glm::ivec2 viewImageSize(const ViewImagePlan& plan, glm::ivec2 viewSize) noexcept;
/// True when a texture of one format can view the storage of the other (glTextureView): uncompressed color formats
/// whose texels have one size. Never for a depth or a block-compressed format.
[[nodiscard]] bool sameViewClass(GpuFormat a, GpuFormat b) noexcept;
/// The bytes the targets of an output of `viewSize` allocate: every image with storage of its own, one level each. What
/// ViewTargets records as GpuMemoryKind::Targets.
[[nodiscard]] std::size_t viewTargetsBytes(glm::ivec2 viewSize) noexcept;

} // namespace rv::gl
