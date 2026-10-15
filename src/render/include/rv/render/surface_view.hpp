#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <array>
#include <cstdint>
#include <string_view>

namespace rv::gl {

/// What the surfaces of a view show: their shading, or one property of their material in false colors (the asset
/// preview's inspection views, drawn with debug/inspect.frag through SceneShaders::inspection and shown without
/// tonemapping, PostSettings::tonemap off).
enum class SurfaceView : std::uint8_t {
    Lit,       ///< the materials' own programs: lit PBR, unlit emissive
    BaseColor, ///< the base color (factor times texture), unlit
    Normals,   ///< the world-space shading normal, normal maps included, as n * 0.5 + 0.5
    UvChecker, ///< a checker over the first uv set, red along u and green along v
};

/// Every view, in the order the asset preview lists them.
inline constexpr std::array<SurfaceView, 4> kSurfaceViews{SurfaceView::Lit, SurfaceView::BaseColor,
                                                          SurfaceView::Normals, SurfaceView::UvChecker};

/// "Lit", "Base color", "Normals" or "UV checker".
[[nodiscard]] std::string_view toString(SurfaceView view) noexcept;

/// The define debug/inspect.frag selects the view with ("RV_INSPECT_BASE_COLOR", "RV_INSPECT_NORMALS",
/// "RV_INSPECT_UV"); empty for SurfaceView::Lit, which has no inspection program.
[[nodiscard]] std::string_view inspectionDefine(SurfaceView view) noexcept;

/// The name of the inspection program of a view in the shader library ("inspect_base_color", "inspect_normals",
/// "inspect_uv_checker"); empty for SurfaceView::Lit.
[[nodiscard]] std::string_view inspectionProgramName(SurfaceView view) noexcept;

} // namespace rv::gl
