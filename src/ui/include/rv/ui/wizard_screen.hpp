#pragma once

// The wizard on assets/ui/wizard.rml (ADR-0018 §4): its data model "wizard" bound to a WizardModel and a FolderBrowserModel
// (the browser over the page, in the same document), the focus on what the models name, the clicks and the wheel. The
// application gives the models the UI commands (NavInput) and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/folder_browser_model.hpp>
#include <rv/ui/wizard_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// A click of the document (the ray's too).
struct WizardClick {
    enum class Kind : std::uint8_t { Button, Browser };
    Kind kind = Kind::Button;
    WizardButton button = WizardButton::Next;
    /// A choice's arrow (-1, +1).
    int direction = 1;
    FolderBrowserClick browser;
};

/// The wizard's document in `view`'s context, over the documents loaded before it. Main thread only; destroy it before its
/// view.
class WizardScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<WizardScreen>> create(UiView& view);
    ~WizardScreen();
    WizardScreen(const WizardScreen&) = delete;
    WizardScreen& operator=(const WizardScreen&) = delete;

    /// Shows the models (bound only when a revision changed). They must stay alive while the screen shows them.
    void show(const WizardModel& model, const FolderBrowserModel& browser);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the focus on what the models name.
    void afterUpdate();
    /// A mouse event while it shows: forwarded to the view; true when it was a button event over a panel.
    bool handlePointer(const SDL_Event& event);
    /// What a click pressed since the last call.
    [[nodiscard]] std::optional<WizardClick> takeClicked() noexcept;
    /// Lines the wheel scrolled the browser's list since the last call (positive: down).
    [[nodiscard]] int takeWheel() noexcept;
    /// Elements of the page's panel, and of the browser's, whose text does not fit its box as shown now.
    [[nodiscard]] std::uint32_t textCut() const;
    [[nodiscard]] std::uint32_t browserTextCut() const;

private:
    struct Impl;

    explicit WizardScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
