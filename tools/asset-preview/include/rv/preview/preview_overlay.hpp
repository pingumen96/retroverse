#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/preview/preview_subject.hpp>
#include <rv/scene/debug_lines.hpp>

namespace rv::preview {

/// The overlays of the asset preview, each toggled in the Asset panel.
struct OverlaySelection {
    /// Floor grid just below y = 0 (gridHeight): a line every 0.1 m (subjects up to kMaxMinorGridDiagonal across),
    /// brighter every 1 m, X and Z axes in color.
    bool grid = true;
    bool scaleReferences = true; ///< a 1 m wireframe cube and a 1.70 m human-height marker beside the subject
    bool bounds = true;          ///< the subject's bounds, with its dimensions in centimeters
    bool nodeBounds = false;     ///< the bounds of every drawn node of a model (of every part of a prefab)
    bool pivot = true;           ///< the axes of the subject's origin, its pivot
    /// Socket axes and names, screen outlines, slot and level volumes (no names), collision proxies as outlines.
    bool sockets = true;
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
/// The volume of a slot socket (Slot_<level>_<index>): what one item of the collection fills.
inline constexpr glm::vec4 kSlot{0.55f, 0.85f, 0.3f, 1.0f};
/// The free volume of a level socket (Level_<level>): where shelf dressing stands when no title needs the level.
inline constexpr glm::vec4 kLevel{0.3f, 0.55f, 0.6f, 1.0f};
inline constexpr glm::vec4 kCollider{0.95f, 0.35f, 0.7f, 1.0f};
inline constexpr glm::vec4 kNormals{0.35f, 0.45f, 1.0f, 1.0f};
} // namespace overlay_colors

/// Spacing of the floor grid's lines, meters.
inline constexpr float kGridMinorSpacing = 0.1f;
inline constexpr float kGridMajorSpacing = 1.0f;
/// Subjects with bounds up to this diagonal, meters, get the 10 cm lines: framed whole, they show them at least about 5
/// pixels apart in the view the panels leave. Larger subjects (a room) get the 1 m lines only, as their 10 cm lines
/// would alias into a moire.
inline constexpr float kMaxMinorGridDiagonal = 6.0f;
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
/// Height of the floor grid's plane for a subject with `bounds`: 0.4% of the subject's diagonal below y = 0 (between
/// 0.01 mm and 10 cm), so that a floor at y = 0 (a room's) hides the grid while the subject is framed whole, despite
/// the pull towards the eye that lets debug lines win over the surfaces they lie on; the gap shows as a pixel or two.
[[nodiscard]] float gridHeight(const core::Aabb& bounds) noexcept;

/// What the camera frames for `subject`: its bounds alone (`surroundings` false: F, a new subject), or together with
/// what the overlays of `selection` show around them (F again, the empty preview): the socket origins (sockets on: an
/// anchor in front of a model, as the CRT's Anchor_ScreenLook) and the 1 m cube and the 1.70 m marker (scale on). An
/// empty preview, or one whose bounds cannot be measured, counts as a 1 m cube standing on the origin.
[[nodiscard]] core::Aabb framingBounds(const PreviewSubject& subject, const OverlaySelection& selection,
                                       bool surroundings);

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

/// How the labels of the scene are laid out on screen, in pixels: each text on a plate `padding` larger on every
/// side, the plate's bottom-left corner `offset` right of and above the labelled point, which shows a dot of
/// `dotRadius` (smaller than offset - padding, so that a plate never covers its own dot).
struct LabelLayout {
    float padding = 3.0f;
    float offset = 6.0f;
    float dotRadius = 2.5f;
};

/// Places the plates of labels whose texts measure `textSizes` (pixels) at the screen points `anchors` (origin
/// top-left, +Y down), in order: each in the nearest place of its column, above and right of its point, then one plate
/// height (plus a pixel) down, one up, two down, two up and so on (as many each way as there are labels), where it
/// covers neither a plate placed before it nor the dot of any label (the last place tried when none is free). `plates`
/// receives one rectangle (x0, y0, x1, y1) per label of the shorter list, its storage reused from call to call.
void placeLabelPlates(std::span<const glm::vec2> anchors, std::span<const glm::vec2> textSizes,
                      const LabelLayout& layout, std::vector<glm::vec4>& plates);
/// The plate of a label at `anchor` left its home place (placeLabelPlates moved it): it no longer straddles the line
/// `offset` above its point. A leader line then joins the point to the plate.
[[nodiscard]] bool plateMoved(const glm::vec4& plate, const glm::vec2& anchor, const LabelLayout& layout) noexcept;

} // namespace rv::preview
