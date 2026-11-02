#pragma once

// The emulators screen on assets/ui/emulators.rml (ADR-0018 §5): its data model "emulators" bound to an EmulatorsModel and,
// over it, a FolderBrowserModel choosing a program; the focus on the element the models name, the clicks. The application
// gives the models the UI commands (NavInput) and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/emulators_model.hpp>
#include <rv/ui/folder_browser_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// A click on the screen: on it, or on the browser over it.
struct EmulatorsScreenClick {
    bool browser = false;
    EmulatorsClick screen;
    FolderBrowserClick folder;
};

/// The screen's document in `view`'s context, over the documents loaded before it. Main thread only; destroy it before
/// its view.
class EmulatorsScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<EmulatorsScreen>> create(UiView& view);
    ~EmulatorsScreen();
    EmulatorsScreen(const EmulatorsScreen&) = delete;
    EmulatorsScreen& operator=(const EmulatorsScreen&) = delete;

    /// Shows the models (bound only when a revision changed), brought to the front when it was hidden. They must stay
    /// alive while the screen shows them.
    void show(const EmulatorsModel& model, const FolderBrowserModel& browser);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the focus on the element the models name, scrolled into view.
    void afterUpdate();
    /// A mouse event while it shows: forwarded to the view; true when it was a button event over the panel.
    bool handlePointer(const SDL_Event& event);
    /// The click since the last call, and the wheel's notches over the browser's rows.
    [[nodiscard]] std::optional<EmulatorsScreenClick> takeClicked() noexcept;
    [[nodiscard]] int takeWheel() noexcept;
    /// The element that has RmlUi's focus ("" none).
    [[nodiscard]] std::string focusedElement() const;
    /// Elements of the panel, of the browser, whose text does not fit its box as shown now.
    [[nodiscard]] std::uint32_t textCut() const;
    [[nodiscard]] std::uint32_t browserTextCut() const;

private:
    struct Impl;

    explicit EmulatorsScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
