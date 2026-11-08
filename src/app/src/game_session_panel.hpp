#pragma once

#include <functional>
#include <memory>

#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/core/expected.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {
class UiView;
} // namespace rv::ui

namespace rv::app {

/// What the panel's buttons ask of the application. Every callback is optional.
struct GameSessionPanelEvents {
    /// "Torna a Retroverse" (Playing, StillRunning), "Sono tornato" (Confirm).
    std::function<void()> returnHome;
    /// "Torna al gioco", "Continua ad aspettare".
    std::function<void()> resume;
    /// The outcome's "OK".
    std::function<void()> dismiss;
};

/// The game-session panel over the quick catalog (assets/ui/game_session.rml, game_session_ui.hpp): the phases of
/// SessionPanel, the game's title, the outcome's LaunchMessage, bound to the RmlUi data model "session" of `view`'s
/// context. A modal document: shown, it takes the focus (its first choice) and the mouse; hidden, it gives the focus
/// back to the application, which refocuses the catalog. Commands: the directions move the focus among the buttons
/// (RmlUi's spatial navigation, within their row), Activate presses the focused one; Back is the application's
/// (LaunchFlow::panelBack). Main thread only; destroy it before its view.
class GameSessionPanel {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<GameSessionPanel>> create(ui::UiView& view,
                                                                            GameSessionPanelEvents events);
    ~GameSessionPanel();
    GameSessionPanel(const GameSessionPanel&) = delete;
    GameSessionPanel& operator=(const GameSessionPanel&) = delete;

    /// Shows `panel` (SessionPanel::None hides it) for the game `title` with `message` (the Outcome phase's) and the
    /// window timeout `timeoutSeconds` (StillRunning's text), laid out at once and its first choice focused.
    void show(SessionPanel panel, std::string_view title, const LaunchMessage& message, int timeoutSeconds);
    void hide();
    [[nodiscard]] SessionPanel shown() const noexcept;
    [[nodiscard]] bool visible() const noexcept { return shown() != SessionPanel::None; }
    /// The panel's command (ignored while hidden): directions and Activate.
    void apply(ui::UiCommand command);
    /// The high-contrast theme (specification §12.4).
    void setHighContrast(bool enabled);
    /// I11 step 13 (ADR-0020 §5): the running game is in exclusive fullscreen; the panel says so (model "exclusive").
    void setExclusiveFullscreen(bool exclusive);
    /// I11 step 14: Playing asked in VR after the user stopped watching ("Torna al gioco" focused first), and the game
    /// playing on Windows' default audio output. Set before show().
    void setWatchStopped(bool stopped);
    void setAudioElsewhere(bool elsewhere);

private:
    struct Impl;

    explicit GameSessionPanel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
