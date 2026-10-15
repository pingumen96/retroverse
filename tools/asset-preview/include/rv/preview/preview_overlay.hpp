#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/preview/preview_subject.hpp>
#include <rv/scene/debug_lines.hpp>

namespace rv::preview {

/// The overlays of the asset preview, each toggled in the Asset panel.
struct OverlaySelection {
    bool grid = true;            ///< floor grid on y = 0: a line every 0.1 m, brighter every 1 m, X and Z axes in color
    bool scaleReferences = true; ///< a 1 m wireframe cube and a 1.70 m human-height marker beside the subject
    bool bounds = true;          ///< the subject's bounds, with its dimensions in centimeters
    bool nodeBounds = false;     ///< the bounds of every drawn node of a model (of every part of a prefab)
    bool pivot = true;           ///< the axes of the subject's origin, its pivot
    bool sockets = true;         ///< socket axes and names, screen outlines, collision proxies as outlines
    bool normals = false;        ///< a short line along the normal of every vertex

    friend bool operator==(const OverlaySelection&, const OverlaySelection&) = default;
};

/// A text placed in the scene, drawn over it by the developer overlay.
struct SceneLabel {
    glm::vec3 position{0.0f};
    std::string text;
    /// sRGB color of the text, with alpha.
    glm::vec4 color{1.0f};
};

/// The lines and labels of the overlays of a subject.
struct PreviewOverlay {
    scene::DebugLines lines;
    std::vector<SceneLabel> labels;
};

/// Linear colors of the preview's lines (the subject's bounds are debug_colors::kObjectBounds, axes red, green, blue).
namespace overlay_colors {
inline constexpr glm::vec4 kGridMinor{0.12f, 0.12f, 0.12f, 1.0f};
inline constexpr glm::vec4 kScaleReference{0.55f, 0.55f, 0.52f, 1.0f};
inline constexpr glm::vec4 kNodeBounds{0.25f, 0.62f, 0.75f, 1.0f};
inline constexpr glm::vec4 kScreen{0.2f, 0.9f, 0.95f, 1.0f};
inline constexpr glm::vec4 kCollider{0.95f, 0.35f, 0.7f, 1.0f};
inline constexpr glm::vec4 kNormals{0.35f, 0.45f, 1.0f, 1.0f};
} // namespace overlay_colors

/// Spacing of the floor grid's lines, meters.
inline constexpr float kGridMinorSpacing = 0.1f;
inline constexpr float kGridMajorSpacing = 1.0f;
/// Height of the human-height marker, meters.
inline constexpr float kHumanHeight = 1.7f;
/// Most vertex normals drawn: a model with more vertices shows every n-th one.
inline constexpr std::size_t kMaxNormalLines = 20000;
/// Most outline edges of collision proxies drawn.
inline constexpr std::size_t kMaxColliderEdges = 20000;

/// Bounds of the 1 m reference cube beside a subject with `bounds`: standing on the floor left of it (-X), 25 cm away
/// (or a tenth of the subject's width when wider), centered on its depth.
[[nodiscard]] core::Aabb referenceCubeBounds(const core::Aabb& bounds) noexcept;
/// Half extent of the floor grid around a subject with `bounds`: whole meters reaching past the subject and its scale
/// references, at least 2 m, at most 50 m.
[[nodiscard]] float gridHalfExtent(const core::Aabb& bounds) noexcept;

/// The lines and labels of `selection` for `subject` (world space = the subject's space). Labels: the dimensions at the
/// middle of three edges of the bounds ("W 71.0 cm"; none on the magenta placeholder, whose size is not the asset's),
/// the socket names at their origins (shelf slots, too many to name, show their axes only), "pivot", "1 m cube" and
/// "1.70 m" on the scale references.
[[nodiscard]] PreviewOverlay buildPreviewOverlay(const PreviewSubject& subject, const OverlaySelection& selection);

/// "71.0 cm": meters as centimeters with one decimal.
[[nodiscard]] std::string formatCentimeters(float meters);
/// "71.0 x 61.0 x 56.0 cm": width (x), height (y) and depth (z).
[[nodiscard]] std::string formatDimensions(const glm::vec3& size);

/// Where `point` appears in a viewport of `size` (origin top-left, +Y down, as Dear ImGui places things) through
/// `viewProjection`: nothing for a point behind the eye or outside the view volume.
[[nodiscard]] std::optional<glm::vec2> projectToViewport(const glm::mat4& viewProjection, const glm::vec3& point,
                                                         const glm::vec2& size) noexcept;

} // namespace rv::preview
