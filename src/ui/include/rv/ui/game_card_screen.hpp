#pragma once

// The world's game card on assets/ui/game_card.rml (I3 step 14a; ADR-0012 §5): its data model "game_card", the focus
// on the button its model names (the :focus style), the clicks on its buttons, the cover through the image source
// rv-texture:game-card-cover (the application registers the texture with UiSystem::setTexture).

#include <memory>
#include <optional>

#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/ui/game_card_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// The card's document in `view`'s context: shown without taking the input from the world (the application reads the
/// world's actions and gives the card its focus), its buttons pressed by a click (pointer events the application
/// forwards while the mouse is free). Main thread only; destroy it before its view.
class GameCardScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<GameCardScreen>> create(UiView& view);
    ~GameCardScreen();
    GameCardScreen(const GameCardScreen&) = delete;
    GameCardScreen& operator=(const GameCardScreen&) = delete;

    /// Shows `content` with `focused` focused. The same content and focus again change nothing.
    void show(const GameCardContent& content, CardButton focused);
    void hide();
    [[nodiscard]] bool visible() const noexcept;

    /// A mouse event while the card shows: forwarded to the view (hover, presses); true when it was a button event
    /// over the card, which the room must not see. Other events, and any while it is hidden: false, not forwarded.
    bool handlePointer(const SDL_Event& event);
    /// The button a click pressed since the last call.
    [[nodiscard]] std::optional<CardButton> takeClicked() noexcept;

    /// The card's content is cut by its maximum height (its scroll height beyond its client height): false while it
    /// is hidden.
    [[nodiscard]] bool squeezed() const;
    /// The cover's box as laid out: x, y from the view's top-left corner, width, height, in pixels; nothing while the
    /// card is hidden or shows no cover.
    [[nodiscard]] std::optional<glm::vec4> coverBox() const;

private:
    struct Impl;

    explicit GameCardScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
