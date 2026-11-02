#pragma once

// The on-screen keyboard on assets/ui/keyboard.rml (ADR-0018 §8): its data model "keyboard" bound to a KeyboardModel, the
// clicks on its keys and around its panel. The focus stays on the text field it types into: the selected key is a class
// of the document, not RmlUi's focus.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/ui/keyboard_model.hpp>

namespace rv::ui {

class UiView;

/// A click: the key's index in keyboardKeys(), none for a click outside the panel.
struct KeyboardClick {
    std::optional<std::size_t> key;
};

/// The keyboard's document in `view`'s context, over the documents loaded before it. Main thread only; destroy it before
/// its view.
class KeyboardScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<KeyboardScreen>> create(UiView& view);
    ~KeyboardScreen();
    KeyboardScreen(const KeyboardScreen&) = delete;
    KeyboardScreen& operator=(const KeyboardScreen&) = delete;

    /// Shows `model` (bound only when its revision changed), brought to the front when it was hidden. The model must stay
    /// alive while the screen shows it.
    void show(const KeyboardModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// The click since the last call.
    [[nodiscard]] std::optional<KeyboardClick> takeClicked() noexcept;
    /// Elements of the panel whose text does not fit its box as shown now.
    [[nodiscard]] std::uint32_t textCut() const;
    /// The grid's border box in the view's pixels, as UiView::elementBox.
    [[nodiscard]] std::optional<glm::vec4> gridBox() const;

private:
    struct Impl;

    explicit KeyboardScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
