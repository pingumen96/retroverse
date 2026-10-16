#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rv::ui {

/// An element that can hold the focus: the region of the screen it belongs to (a list, a row of filters, a row of
/// actions; the document marks it) and its element id.
struct FocusTarget {
    std::string region;
    std::string element;

    bool operator==(const FocusTarget&) const = default;
};

/// Where Back returns the focus (style §16.3 "Indietro"): the element the focus left each time it moved into another
/// region, along the path the user took. Moving into a region that is already on the path cuts the path back to it, so
/// the history holds at most one entry per region and Back retraces the user's steps (the detail actions back to the
/// game they were opened from, the filters back to the list). An empty history means Back leaves the screen.
///
/// The move that back() causes is not recorded as a new step: the next onFocusMoved() into the region back() returned
/// is taken as that return. Moves within a region are not recorded either. Main thread only.
class FocusHistory {
public:
    /// Most entries kept: deeper paths forget their oldest step.
    static constexpr std::size_t kMaxDepth = 16;

    /// The focus moved from `from` (nothing: no element had it) to an element of region `toRegion`.
    void onFocusMoved(const std::optional<FocusTarget>& from, std::string_view toRegion);
    /// Removes and returns the element Back returns to; nothing when the history is empty (Back leaves the screen).
    [[nodiscard]] std::optional<FocusTarget> back();
    /// The element Back would return to, without removing it.
    [[nodiscard]] const FocusTarget* peek() const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept { return m_path.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_path.empty(); }
    /// Forgets the path (a screen opening anew).
    void clear() noexcept;

private:
    std::vector<FocusTarget> m_path;
    /// The region back() returned to, until the focus arrives there.
    std::optional<std::string> m_returningTo;
};

} // namespace rv::ui
