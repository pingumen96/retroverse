#pragma once

// The settings screen on assets/ui/settings.rml (ADR-0018 §3): its data model "settings" bound to a SettingsModel (the
// group, its rows, what each holds), the focus on the element the model names, the clicks. The application gives the
// model the UI commands (NavInput) and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/settings_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// The screen's document in `view`'s context, over the documents loaded before it. Main thread only; destroy it before
/// its view.
class SettingsScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<SettingsScreen>> create(UiView& view);
    ~SettingsScreen();
    SettingsScreen(const SettingsScreen&) = delete;
    SettingsScreen& operator=(const SettingsScreen&) = delete;

    /// Shows `model` (bound only when its revision changed), brought to the front when it was hidden. The model must stay
    /// alive while the screen shows it.
    void show(const SettingsModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the focus on the element the model names, scrolled into the list's view.
    void afterUpdate();
    /// A mouse event while it shows: forwarded to the view; true when it was a button event over the panel.
    bool handlePointer(const SDL_Event& event);
    /// The click since the last call.
    [[nodiscard]] std::optional<SettingsClick> takeClicked() noexcept;
    /// The element that has RmlUi's focus ("" none).
    [[nodiscard]] std::string focusedElement() const;
    /// The focused row lies outside the list's visible part (under a capture's strip, say).
    [[nodiscard]] bool focusedRowCovered() const;
    /// Elements of the panel whose text does not fit its box as shown now.
    [[nodiscard]] std::uint32_t textCut() const;

private:
    struct Impl;

    explicit SettingsScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
