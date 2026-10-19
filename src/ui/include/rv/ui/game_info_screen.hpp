#pragma once

// Informazioni on assets/ui/game_info.rml (I3 step 14b; ADR-0012 §5): its data model "game_info" bound to a
// GameInfoContent (the game's metadata, its description and the other versions of its title), the body scrolled by the
// UI commands, the clicks on its buttons.

#include <memory>
#include <optional>

#include <rv/core/expected.hpp>
#include <rv/ui/game_info_model.hpp>

namespace rv::ui {

class UiView;

/// The document in `view`'s context, over every other document while it shows. It never takes RmlUi's focus (the
/// application gives it the UI commands); pointer events reach it through the view (the wheel scrolls its body). Main
/// thread only; destroy it before its view.
class GameInfoScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<GameInfoScreen>> create(UiView& view);
    ~GameInfoScreen();
    GameInfoScreen(const GameInfoScreen&) = delete;
    GameInfoScreen& operator=(const GameInfoScreen&) = delete;

    /// Shows `content` (bound only when it changed; another game scrolls back to the top), brought to the front when
    /// it was hidden.
    void show(const GameInfoContent& content);
    void hide();
    [[nodiscard]] bool visible() const noexcept;

    /// Scrolls the body (the scrolling steps of InfoStep; others do nothing). True when it moved.
    bool scroll(InfoStep step);
    /// The body's scroll offset and its content's height beyond its own (0: all of it shows), in pixels.
    [[nodiscard]] float scrollTop() const;
    [[nodiscard]] float scrollRange() const;
    /// The button a click pressed since the last call: Close or Media.
    [[nodiscard]] std::optional<InfoStep> takeClicked() noexcept;

private:
    struct Impl;

    explicit GameInfoScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
