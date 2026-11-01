#pragma once

// The menu on assets/ui/system_menu.rml (ADR-0018 §1): its data model "system_menu" bound to a SystemMenuModel, the focus
// on the button the model names, the clicks on its buttons. The application gives the model the UI commands (NavInput)
// and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/system_menu_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// The menu's document in `view`'s context, over the documents loaded before it. Main thread only; destroy it before its
/// view.
class SystemMenuScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<SystemMenuScreen>> create(UiView& view);
    ~SystemMenuScreen();
    SystemMenuScreen(const SystemMenuScreen&) = delete;
    SystemMenuScreen& operator=(const SystemMenuScreen&) = delete;

    /// Shows `model` (bound only when its revision changed), brought to the front when it was hidden. The model must stay
    /// alive while the screen shows it.
    void show(const SystemMenuModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the focus on the button the model names.
    void afterUpdate();
    /// A mouse event while it shows: forwarded to the view; true when it was a button event over the panel.
    bool handlePointer(const SDL_Event& event);
    /// The button a click pressed since the last call.
    [[nodiscard]] std::optional<SystemMenuButton> takeClicked() noexcept;
    /// The element that has RmlUi's focus ("" none).
    [[nodiscard]] std::string focusedElement() const;
    /// Elements of the panel whose text does not fit its box as shown now.
    [[nodiscard]] std::uint32_t textCut() const;

private:
    struct Impl;

    explicit SystemMenuScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
