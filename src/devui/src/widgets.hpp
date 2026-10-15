#pragma once

#include <cstddef>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Widgets shared by the panels of the developer overlay (Dear ImGui, inside an overlay frame).
namespace rv::devui::widgets {

/// Color picker for a linear RGB color (lights, ambient, background radiance): shown and edited as an sRGB hex code,
/// the way colors are authored (style §3; the picker and the context menu offer the other forms), and written back
/// converted to linear. Components are limited to [0, 1]. True when edited.
bool linearColor(const char* label, glm::vec3& linear);
/// Same for linear RGBA (base colors, "#RRGGBBAA"): alpha is shown and kept as is.
bool linearColor(const char* label, glm::vec4& linear);

/// Unit direction editor (spot and directional lights): the vector is renormalized after each edit, and an edit that
/// would make it zero or non-finite is undone. True when edited.
bool direction(const char* label, glm::vec3& direction);

/// Narrows the following widgets so that labels up to `labelEms` font sizes wide fit on their right, also in narrow
/// docked panels. Undo with ImGui::PopItemWidth().
void pushLabelWidth(float labelEms = 8.0f);

/// A "label: value" row of a two-column table started with beginValueTable().
void valueRow(const char* label, std::size_t value);
/// Starts a compact two-column table (labels, values) with the given id; end it with ImGui::EndTable() when it returns
/// true.
bool beginValueTable(const char* id);

/// Shows `bytes` as B, KiB or MiB in the current table cell or line.
void bytes(std::size_t bytes);

} // namespace rv::devui::widgets
