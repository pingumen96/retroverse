#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <string>
#include <string_view>

#include <rv/assets/model_data.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

/// Name of the --model viewer scene and id of its only object, the model.
inline constexpr std::string_view kModelViewerSceneName = "model_viewer";
inline constexpr std::string_view kModelViewerObjectId = "model";

/// Aspect ratio the viewer camera frames the model for (the default window and screenshot sizes are 16:9).
inline constexpr float kModelViewerAspect = 16.0f / 9.0f;
/// Directions the viewer looks at the model from: this far to the right of its front (+Z) and above it, in degrees.
inline constexpr float kModelViewerAzimuthDeg = 35.0f;
inline constexpr float kModelViewerElevationDeg = 25.0f;
/// Distance factor over the tight fit of the bounding sphere, leaving a margin around the model.
inline constexpr float kModelViewerMargin = 1.15f;

/// Scene of the --model viewer (spec plan 5.7): the model `modelPath` (as given: the viewer's model loader resolves
/// it) at the origin, unrotated, so its own axes are the world's (ADR-0002: +Y up, front +Z, meters). The model does
/// not collide: free roam can get as close to it as inspecting it needs.
///  - Camera: the default lens (75 degrees horizontal), looking at the center of `bounds` (the model's bounds) from
///    kModelViewerAzimuthDeg to the right of its front and kModelViewerElevationDeg above, far enough that the bounding
///    sphere fits a kModelViewerAspect view with kModelViewerMargin; near and far planes follow the distance, so
///    millimeter details and 20 m models both fit.
///  - Lights, a three-point rig sized to the bounds: a warm directional key light from the front left above, a cool
///    point fill light from the front right and a point rim light from behind above (their intensities scale with the
///    square of their distance, so the model receives the same light whatever its size).
///  - Environment: neutral hemispheric ambient, exposure 1, a neutral dark mid-grey background (shown as about sRGB
///    61), against which dark models (black plastic, graphite) and light ones both stand out.
/// Empty or non-finite bounds are framed as a 1 m cube standing on the origin.
[[nodiscard]] scene::SceneDescription makeModelViewerScene(std::string modelPath, const core::Aabb& bounds);

/// Reference grid of the debug lines (scene::DebugLines::addGrid) on the y = 0 plane.
struct DebugGrid {
    float halfExtent = 10.0f; ///< meters from the origin along x and z
    float spacing = 1.0f;     ///< meters between lines
};

/// Grid for a model with `bounds` in the viewer, so that the model spans 2 to 20 cells whatever its size: the spacing
/// is the power of ten at or below half the model's largest extent (a 0.5 m model gets 10 cm cells, a 12 m one 1 m
/// cells, a 1 cm part 1 mm cells), and the grid reaches twice the model's horizontal distance from the origin, at least
/// 20 cells. Empty or non-finite bounds are taken as a 1 m cube.
[[nodiscard]] DebugGrid modelViewerGrid(const core::Aabb& bounds) noexcept;

/// One-line summary of a loaded model for the log: "Model <path>: <n> node(s), <m> mesh(es) with <p> primitive(s), <t>
/// triangle(s), <k> material(s), <i> image(s); bounds <x> x <y> x <z> m from (<min>) to (<max>)" (millimeter
/// precision), or "...; no bounds (nothing to draw)" for a model without a drawn primitive.
[[nodiscard]] std::string describeModel(std::string_view path, const assets::ModelData& model);

} // namespace rv::app
