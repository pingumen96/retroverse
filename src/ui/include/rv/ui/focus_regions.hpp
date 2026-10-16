#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/focus_history.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// The regions of a screen and how the focus crosses between them. Inside a region the UI's spatial navigation moves
/// the focus (RmlUi's nav properties, confined to the region's scroll container); at a region's edge, where it finds
/// nothing further in a direction, a link names the region the focus moves to. There the focus lands on the element
/// that last had it in that region (TV menus bring the user back where they were), else on the region's entry element
/// (the selected game, the active filter), which the screen keeps current. Regions are few: plain vectors, linear
/// lookups. Main thread only.
class FocusRegions {
public:
    /// From `region`, `direction` leads to region `to` (replacing an earlier link). Regions are created on first use.
    void link(std::string_view region, NavDirection direction, std::string_view to);
    /// The element the focus enters `region` on when it remembers none.
    void setEntry(std::string_view region, std::string_view element);
    /// Records that `target` has the focus: its region remembers it.
    void onFocused(const FocusTarget& target);
    /// Forgets the element `region` remembers (it left the document: a list filtered), so the entry is used again.
    void forget(std::string_view region);

    /// The region `direction` leads to from `region`, if linked.
    [[nodiscard]] std::optional<std::string> linked(std::string_view region, NavDirection direction) const;
    /// The element the focus lands on in `region`: the remembered one, else the entry; nothing when it has neither.
    [[nodiscard]] std::optional<std::string> landing(std::string_view region) const;
    /// The element `region` remembers, if any.
    [[nodiscard]] std::optional<std::string> remembered(std::string_view region) const;
    /// The entry element of `region`, if set.
    [[nodiscard]] std::optional<std::string> entry(std::string_view region) const;
    /// Where the focus goes from `region` in `direction`: the linked region and the element it lands on there.
    [[nodiscard]] std::optional<FocusTarget> across(std::string_view region, NavDirection direction) const;

private:
    struct Region {
        std::string name;
        std::string entry;
        std::string remembered;
        /// Linked region per direction (NavDirection order); empty for none.
        std::array<std::string, 4> links;
    };

    [[nodiscard]] Region& regionNamed(std::string_view name);
    [[nodiscard]] const Region* find(std::string_view name) const noexcept;

    std::vector<Region> m_regions;
};

} // namespace rv::ui
