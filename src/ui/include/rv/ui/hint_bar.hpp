#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The hints from the active map (ADR-0018 §7): what a document's hint shows for an action, on the device used last. A hint's
// verb stays in the document; only the key's name is data (assets/ui/key_labels.json). A document declares one thus, with
// hint_bar.rcss linked after its own style sheet, and nothing else:
//
//     <span class="hint"><span class="pad" hint-action="ui_activate"></span>Avvia</span>
//     <span class="hint"><span class="pad" hint-action="ui_filter_previous ui_filter_next"></span>Gruppo</span>
//     <p>Il catalogo rapido<span class="hint-prose"> (<span hint-action="quick_catalog"></span>)</span> è pronto.</p>
//
// `hint-action` lists names of input::Action. ui::HintBarView fills every such element of the view's documents: its text,
// the class of the pad's button colour (pad-a, pad-b, pad-x, pad-y) for a face button alone, hint-wide for anything longer.
// A hint no action of which has a binding on the device hides: its nearest `.hint` or `.hint-prose`.

#include <cstdint>
#include <string>
#include <string_view>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding_labels.hpp>

namespace rv::ui {

namespace hint_bar {
inline constexpr std::string_view kActionAttribute = "hint-action";
inline constexpr std::string_view kWideClass = "hint-wide";
inline constexpr std::string_view kUnboundClass = "hint-unbound";
inline constexpr std::string_view kHintClass = "hint";
inline constexpr std::string_view kProseClass = "hint-prose";
} // namespace hint_bar

/// What a hint's glyph shows.
struct HintGlyph {
    /// The key's or the buttons' names; empty when `bound` is false.
    std::string text;
    /// A face button's colour class, or empty.
    std::string colour;
    /// Not a round face button: a name's width.
    bool wide = false;
    bool bound = false;

    bool operator==(const HintGlyph&) const = default;
};

class HintBar {
public:
    void setLabels(input::KeyLabels labels);
    void setMap(input::ActionMap map);
    void setDevice(input::DeviceKind device) noexcept;

    [[nodiscard]] input::DeviceKind device() const noexcept { return m_device; }
    [[nodiscard]] const input::ActionMap& map() const noexcept { return m_map; }

    /// The glyph of a `hint-action` value: action names, space-separated. A name that is no action counts for nothing
    /// (a typo shows as an unbound hint, and is logged by HintBarView).
    [[nodiscard]] HintGlyph glyph(std::string_view actions) const;
    /// `action`'s name on `device`, "" when it has no binding there.
    [[nodiscard]] std::string label(input::Action action, input::DeviceKind device) const;
    /// label() for a prompt that must read: the other device's name when `device` has none.
    [[nodiscard]] std::string promptLabel(input::Action action, input::DeviceKind device) const;
    /// Changes with whatever glyph() answers.
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    input::KeyLabels m_labels;
    input::ActionMap m_map;
    input::DeviceKind m_device = input::DeviceKind::KeyboardMouse;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
