#pragma once

// The directory on assets/ui/directory.rml (I4 step 7a; ADR-0013 §10): its data model "directory" bound to a
// DirectoryModel (the floors, a floor's hall and rooms, the favourites and recent games, a game's card), the focus on
// the tab, row or card button the model names (the :focus style; the list scrolled to it), the clicks on its tabs, rows
// and buttons. The application gives the model the UI commands (NavInput) and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/directory_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// The directory's document in `view`'s context, over the documents loaded before it. It never takes the input from
/// the application: the model's focus is applied after each update. Main thread only; destroy it before its view.
class DirectoryScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<DirectoryScreen>> create(UiView& view);
    ~DirectoryScreen();
    DirectoryScreen(const DirectoryScreen&) = delete;
    DirectoryScreen& operator=(const DirectoryScreen&) = delete;

    /// Shows `model` (bound only when its revision changed), brought to the front when it was hidden. The model must
    /// stay alive while the screen shows it (afterUpdate() reads its focus).
    void show(const DirectoryModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the focus on the element the model names (the rows a new binding made exist only after
    /// the update), the list scrolled to it.
    void afterUpdate();

    /// A mouse event while the panel shows: forwarded to the view (hover, clicks); true when it was a button event over
    /// one of the directory's panels, which the world must not see. Other events, and any while it is hidden: false,
    /// not forwarded.
    bool handlePointer(const SDL_Event& event);
    /// The tab, row or card button a click pressed since the last call (DirectoryModel::click takes it).
    [[nodiscard]] std::optional<DirectoryClick> takeClicked() noexcept;

    /// The element that has RmlUi's focus ("" none): the model's, once afterUpdate() applied it.
    [[nodiscard]] std::string focusedElement() const;
    /// The card's content is cut by its maximum height: false while no card shows.
    [[nodiscard]] bool cardSqueezed() const;
    /// Binds the model counted since the screen was created (a revision that changed).
    [[nodiscard]] std::uint64_t binds() const noexcept;

private:
    struct Impl;

    explicit DirectoryScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
