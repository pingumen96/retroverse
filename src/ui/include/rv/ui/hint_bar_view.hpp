#pragma once

// The hints of a view's documents (ADR-0018 §7): every element with a `hint-action` attribute (ui/hint_bar.hpp) filled from
// a ui::HintBar, when the bar or the view's documents change. Screens need do nothing: a document made later is found.
// Main thread only.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Element;
class ElementDocument;
} // namespace Rml

namespace rv::ui {

class HintBar;
class UiView;

/// A hint as shown.
struct HintShown {
    /// The element's `hint-action`.
    std::string actions;
    std::string text;

    bool operator==(const HintShown&) const = default;
};

class HintBarView {
public:
    explicit HintBarView(UiView& view) : m_view(view) {}

    /// Every hint up to date with `bar`. Cheap while nothing changed: a frame calls it.
    void sync(const HintBar& bar);
    /// The hints visible now, in the documents' order.
    [[nodiscard]] std::vector<HintShown> shown() const;
    /// Visible hints that leave the box that holds them (text a panel cuts or that runs past its edge).
    [[nodiscard]] std::uint32_t outside() const;

private:
    UiView& m_view;
    std::optional<std::uint64_t> m_revision;
    std::vector<Rml::ElementDocument*> m_documents;
};

} // namespace rv::ui
