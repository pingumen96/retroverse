#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <cstdint>
#include <optional>
#include <string_view>

#include <rv/render/surface_view.hpp>

namespace rv::preview {

/// What the asset preview draws the surfaces with: the scene's own shading, an inspection view (gl::SurfaceView), or
/// the triangles' edges (colored by their normals, so they show on dark and light materials alike).
enum class ViewMode : std::uint8_t { Lit, BaseColor, Normals, UvChecker, Wireframe };

/// Every view mode, in menu order (keys 1 to 5).
inline constexpr ViewMode kViewModes[] = {ViewMode::Lit, ViewMode::BaseColor, ViewMode::Normals, ViewMode::UvChecker,
                                          ViewMode::Wireframe};

/// "Lit", "Base color", "Normals", "UV checker", "Wireframe".
[[nodiscard]] std::string_view toString(ViewMode mode) noexcept;
/// The command line name of a mode (--view): "lit", "base-color", "normals", "uv-checker", "wireframe".
[[nodiscard]] std::string_view viewModeName(ViewMode mode) noexcept;
/// The mode called `name` (viewModeName), nothing for another name.
[[nodiscard]] std::optional<ViewMode> viewModeFromName(std::string_view name) noexcept;
/// The surface view a mode draws with: Normals for Wireframe.
[[nodiscard]] gl::SurfaceView surfaceViewOf(ViewMode mode) noexcept;
/// The mode draws the triangles' edges (gl::RenderView::wireframe).
[[nodiscard]] bool wireframeOf(ViewMode mode) noexcept;

} // namespace rv::preview
