#pragma once

// ADR-0018 §15: menus and rows are checked for cut text as the card is (GameCardScreen::squeezed()'s rule).

#include <cstdint>

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>

namespace rv::ui::detail {

/// How many elements under `root` (it included) hold more than their box shows without being able to scroll to it:
/// `overflow: hidden` text, a panel squeezed by its maximum height. An element that scrolls loses nothing.
[[nodiscard]] inline std::uint32_t elementsWithCutText(Rml::Element& root) {
    std::uint32_t cut = 0;
    const bool scrolls = root.GetComputedValues().overflow_y() == Rml::Style::Overflow::Auto ||
                         root.GetComputedValues().overflow_y() == Rml::Style::Overflow::Scroll;
    const bool hidden = root.GetComputedValues().overflow_x() == Rml::Style::Overflow::Hidden ||
                        root.GetComputedValues().overflow_y() == Rml::Style::Overflow::Hidden;
    if (hidden && !scrolls && root.IsVisible() &&
        (root.GetScrollWidth() > root.GetClientWidth() + 1.0f ||
         root.GetScrollHeight() > root.GetClientHeight() + 1.0f)) {
        ++cut;
    }
    for (int index = 0; index < root.GetNumChildren(); ++index) {
        if (Rml::Element* child = root.GetChild(index)) {
            cut += elementsWithCutText(*child);
        }
    }
    return cut;
}

} // namespace rv::ui::detail
