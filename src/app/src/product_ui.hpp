#pragma once

#include <functional>
#include <memory>

#include <rv/app/mock_catalog.hpp>
#include <rv/core/expected.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>

union SDL_Event;

namespace rv::gl {
class Framebuffer;
class GLDevice;
} // namespace rv::gl

namespace rv::app {

class AppContext;

/// The product UI of retroverse.exe and what it launches (I2 step A1; specification §12.2 QuickLibrary, §13, §15.5
/// ExternalEmulatorRunning; ADR-0005, ADR-0006): RmlUi over the window (ui::UiSystem, one ui::UiView), the quick
/// catalog (ui::QuickCatalogScreen over a ui::QuickCatalogModel over the mock catalog's games in a
/// ui::InMemoryCatalogSource), the game-session panel over it (GameSessionPanel), and the launches: Avvia plans the
/// selected game's launch target with its emulator profile (launcher::planLaunch: the shipped and the user's profiles,
/// the catalog's over them, executables found through the Windows system probe) and hands the request to a LaunchFlow
/// on the Win32 process runner (the automation guard applies there), the window kept behind the emulator
/// (platform::WindowHandover, HandoverPolicy::Keep) and taken back at the return. Hidden windows (--hidden: automated
/// runs) have no hand-over: nothing is visible to hand over or bring back, and the foreground remedies must never raise
/// a window nobody can see.
///
/// The application keeps the input contexts, the Back levels and the host's idle mode: it reads the UI actions only
/// while hasInput() and gives them to update(), turns every UI Back into its own levels (the `back` callback, then
/// panelBack() or catalogBack()), idles the host while wantsIdle() and calls onIdle() on every wake. Drawn below the
/// developer overlay, in screenshots too. Main thread only; destroy it while the OpenGL context is current.
class ProductUi {
public:
    /// Creates RmlUi over `context`'s window, the quick catalog's model over `catalog`'s games (closed: its screen is
    /// made the first time it opens), the panel (hidden) and the emulator profiles in effect (mockCatalogProfiles over
    /// launcher::loadEmulatorProfiles; their issues are logged). `bindings` give the keys RmlUi leaves to the UI
    /// commands (ui::commandKeys). `back` is called for every UI Back command. Errors: RmlUi, the view or the panel's
    /// document cannot be created.
    [[nodiscard]] static Expected<std::unique_ptr<ProductUi>> create(AppContext& context, MockCatalog catalog,
                                                                     const input::ActionMap& bindings,
                                                                     std::function<void()> back);
    ~ProductUi();
    ProductUi(const ProductUi&) = delete;
    ProductUi& operator=(const ProductUi&) = delete;

    [[nodiscard]] bool catalogOpen() const noexcept;
    /// The game-session panel shows (a game running and asking, an outcome).
    [[nodiscard]] bool panelOpen() const noexcept;
    /// The product UI has the input (InputContext::Ui): the catalog or the panel is open.
    [[nodiscard]] bool hasInput() const noexcept { return catalogOpen() || panelOpen(); }

    /// Opens the quick catalog where it was left (its focus and selection), the UI's held directions forgotten; the
    /// first time, its screen is made (a screen that cannot be made is logged and the catalog stays closed).
    void openCatalog();
    /// Closes it; nothing while the panel shows over it.
    void closeCatalog();

    /// An SDL event, while hasInput(): the mouse, text and the keys that are not UI commands go to RmlUi (UiView). True
    /// when the UI used it: the game must not see it.
    bool handleEvent(const SDL_Event& event);
    /// Retroverse's window gained or lost the input focus (the panel asks the user who comes back while a game runs;
    /// a question renders only while the window has the focus). Ignored for hidden windows, which count as focused.
    void focusChanged(bool focused);
    /// A press (a key, a button, a click) reached the window while the host idled; the application drops it. After a
    /// choice to go back to the game made without leaving Retroverse, it brings the question back
    /// (LaunchFlow::userPressed).
    void userPressed();

    /// One frame: follows a session whose panel shows and the window's return, applies the UI commands of
    /// `uiActions` (the frame's actions in the UI context; null: the product UI had no input when the frame began),
    /// shows the panel the launch wants and updates the view at `timeSeconds` (the simulated time).
    void update(const input::ActionState* uiActions, double deltaSeconds, double timeSeconds);
    /// The Back levels the application chose for a UI Back (BackAction::PanelBack, BackAction::CatalogBack). The
    /// panel's Back while a game runs is "Torna al gioco": in the focused window, the foreground goes to the game.
    void panelBack();
    void catalogBack();

    /// The host must idle: a launch waits for the frame boundary, or a game has the screen (LaunchFlow::hostIdle).
    [[nodiscard]] bool wantsIdle() const noexcept;
    /// One wake of the host's idle wait: starts a pending launch (the window handed over before the process runs),
    /// follows the session, takes the window back once it ends.
    void onIdle();
    /// A frame was rendered.
    void frameRendered() noexcept;

    /// Draws the UI over the window's frame, and into a screenshot (scaled to fit), when something shows.
    void renderToWindow(gl::GLDevice& device);
    void renderTo(gl::GLDevice& device, const gl::Framebuffer& target);

    /// The end of the run (IAppLogic::onShutdown): a running launch is left running (LaunchFlow::quit), the catalog and
    /// the launches go into the run report.
    void finish();

private:
    struct Impl;

    explicit ProductUi(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
