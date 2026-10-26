#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/ui/directory_model.hpp>
#include <rv/ui/game_card_model.hpp>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/app/world_label_ui.hpp>

#include "world_notice_panel.hpp"

union SDL_Event;

namespace rv::gl {
class Framebuffer;
class GLDevice;
} // namespace rv::gl

namespace rv::app {

class AppContext;

/// What the application does around a launch (I3 step 11: the hero CRT's decoder, ADR-0012 §6), on the main thread.
struct LaunchHooks {
    /// A launch is about to start: called before the window's hand-over and the process, on the host's wake.
    std::function<void()> starting;
    /// The launch that started ended (the return), or failed to start: called once, when the flow says it returned.
    std::function<void()> ended;
};

/// The product UI of retroverse.exe and what it launches (I2 steps A1 and 13; specification §12.2 QuickLibrary, §13,
/// §15.5 ExternalEmulatorRunning; ADR-0005, ADR-0006): RmlUi over the window (ui::UiSystem, one ui::UiView), the quick
/// catalog (ui::QuickCatalogScreen over a ui::QuickCatalogModel over the AppCatalog's source: the ES-DE catalog through
/// the catalog worker, or the mock catalog of the tests), the game-session panel over it (GameSessionPanel), and the
/// launches: Avvia asks the AppCatalog for its decision (the mock plans the game's launch with its emulator profile at
/// once; the ES-DE catalog resolves it on the worker right before the launch, with the database's choices) and hands a
/// ready request to a LaunchFlow on the Win32 process runner (the automation guard applies there), the window kept
/// behind the emulator (platform::WindowHandover, HandoverPolicy::Keep) and taken back at the return; a game that cannot
/// start shows why. The play sessions are the AppCatalog's (recorded on the worker). Hidden windows (--hidden: automated
/// runs) have no hand-over: nothing is visible to hand over or bring back, and the foreground remedies must never raise
/// a window nobody can see; they count as focused (the input script stands for the user in front of them). Frame-limited
/// runs stop waiting for a game after --launch-wait-limit (LaunchFlow::setWaitLimit). Runs with --fixed-dt wait between
/// frames for what they asked of the catalog worker (lockstep: the host idles until it answered), so that scripted runs
/// see the same lists and launches on the same frames; other runs never wait for it.
///
/// The application keeps the input contexts, the Back levels and the host's idle mode: it reads the UI actions only
/// while hasInput() and gives them to update(), turns every UI Back into its own levels (the `back` callback, then
/// panelBack() or catalogBack()), idles the host while wantsIdle() and calls onIdle() on every wake. Drawn below the
/// developer overlay, in screenshots too. Main thread only; destroy it while the OpenGL context is current.
class ProductUi {
public:
    /// Creates RmlUi over `context`'s window, the quick catalog's model over `catalog`'s source (closed: its screen is
    /// made the first time it opens) and the panel (hidden). `bindings` give the keys RmlUi leaves to the UI commands
    /// (ui::commandKeys). `back` is called for every UI Back command; `pose` gives the walker's pose for the launch
    /// records (LaunchRecord::before, ::after). Errors: RmlUi, the view or the panel's document cannot be created.
    [[nodiscard]] static Expected<std::unique_ptr<ProductUi>> create(AppContext& context,
                                                                     std::unique_ptr<AppCatalog> catalog,
                                                                     const input::ActionMap& bindings,
                                                                     std::function<void()> back,
                                                                     std::function<std::optional<SpawnPoint>()> pose);
    ~ProductUi();
    ProductUi(const ProductUi&) = delete;
    ProductUi& operator=(const ProductUi&) = delete;

    [[nodiscard]] bool catalogOpen() const noexcept;
    /// The game-session panel shows (a game running and asking, an outcome), or the media viewer or Informazioni (I3
    /// step 14b: Back's PanelBack level closes them first).
    [[nodiscard]] bool panelOpen() const noexcept;
    /// The product UI has the input (InputContext::Ui): the catalog, the directory or the panel is open.
    [[nodiscard]] bool hasInput() const noexcept { return catalogOpen() || panelOpen() || directoryOpen(); }

    /// The directory (I4 step 7b; ADR-0013 §10; docs/i4/step-7a.md's contract: ui::DirectoryModel on
    /// assets/ui/directory.rml through ui::DirectoryScreen): open (the UI's input, its Back level), its floors and rooms
    /// from the world (setDirectoryBuilding), its favourites and recent games from the catalog worker
    /// (AppCatalog::requestRecentGames, asked whenever the model asks), its card the quick catalog's (the catalog's row,
    /// readiness and blocker; the room key of setRoomPlacement()), Avvia on it launched here as the quick catalog's
    /// (its outcome back on the card), the world's commands (ChooseFloor, ChooseRoom, ShowInRoom) handed to the world
    /// (takeDirectoryCommands()).
    [[nodiscard]] bool directoryOpen() const noexcept;
    /// Opens the directory on `tab` (`via`: "elevator" or "map", for the run report); its screen is made the first
    /// time (a screen that cannot be made is logged and the directory stays closed).
    void openDirectory(ui::DirectoryTab tab, std::string_view via);
    /// Opens the directory at `game`'s card (I6 step 5: a proxy of the hub's favourites showcase, `via` "showcase"), the
    /// quick catalog's card with Avvia and Mostra nella stanza, its games list behind it. Nothing while the quick
    /// catalog or the directory is open.
    void openDirectoryCard(const ui::DirectoryGame& game, std::string_view via);
    /// Its Back level (BackAction::DirectoryBack): one level back, closing it at its root.
    void directoryBack();
    /// Closes it (Map pressed while it shows); nothing while the panel shows over it.
    void closeDirectory();
    /// The floors and rooms it lists and where the walker is (the same building again changes nothing).
    void setDirectoryBuilding(ui::DirectoryBuilding building);
    /// The world's commands the directory gave since the last call, in order (the panel closed itself for each).
    [[nodiscard]] std::vector<ui::DirectoryCommand> takeDirectoryCommands();
    /// `command` refused for now by the world (a ride in Focus, a plan not ready): the panel again where it was, with
    /// `notice` (ui::directory::kNotices) under its list.
    void refuseDirectory(const ui::DirectoryCommand& command, std::string_view notice);
    /// The elevator's prompt naming `key` from now on (nothing: none), the hub's totem's with `totem` (I6 step 5):
    /// shown over the scene while neither the quick catalog, the directory nor the game-session panel shows. It takes no
    /// input.
    void setElevatorPrompt(std::optional<std::string> key, bool totem = false);

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

    /// Text typed into the product UI's focused text field (an --input-script step's "text"), while hasInput().
    void typeText(std::string_view text);

    /// The application's hooks around launches (replacing any set before).
    void setLaunchHooks(LaunchHooks hooks);
    /// Avvia on an item of the room (I3 step 15a, ADR-0012 §6): `game` launched as the quick catalog's Avvia launches a
    /// game (AppCatalog::requestLaunch: the ES-DE catalog resolves it on its worker right before the launch; then the
    /// LaunchFlow, its play session, the launch hooks). False, and nothing asked, while another launch is being decided,
    /// waits or runs.
    bool launchWorldGame(const catalog::GameId& game, std::string_view title);
    /// How the room's launch asked by launchWorldGame() ended, once: its outcome as the game-session panel tells it
    /// (launch_outcome::kReturned after a normal return; a refusal's or a failure's message otherwise).
    [[nodiscard]] std::optional<LaunchMessage> takeWorldLaunchEnded();
    /// Where a game (its catalog id) stands in the room, for the quick catalog's card (I3 step 15a, ADR-0012 §8:
    /// ui::QuickCatalogScreen::setRoomPlacement's keys); the card shown fills again. Unset: no room.
    void setRoomPlacement(std::function<std::string(std::string_view gameId)> placement);
    /// The game (its catalog id) whose "Mostra nella stanza" was pressed since the last call; the catalog closes at
    /// the end of that frame.
    [[nodiscard]] std::optional<std::string> takeShowInRoom();
    /// The game selected in the quick catalog while it is open (its catalog id); nothing while it is closed or empty.
    [[nodiscard]] std::optional<std::string> selectedGameId() const;

    /// What the world's notice says from now on (I3 step 10b; an empty notice: none): shown over the scene, in
    /// screenshots too, while neither the quick catalog nor the game-session panel shows. It takes no input.
    void setWorldNotice(WorldNoticeContent content);
    /// The hover's title label from now on (I3 step 13a; nothing: none): shown over the scene, in screenshots too,
    /// while neither the quick catalog nor the game-session panel shows. It takes no input.
    void setWorldLabel(std::optional<WorldLabelContent> content);
    /// The world's card beside the inspected item from now on (I3 step 14a; nothing: none), its button `focused`
    /// focused: shown over the scene, in screenshots too, while neither the quick catalog nor the game-session panel
    /// shows. It takes no keys or pad buttons (the world reads them: WorldCard), only clicks while the mouse is free
    /// (handleWorldCardEvent).
    void setWorldCard(std::optional<ui::GameCardContent> content, ui::CardButton focused);
    /// A mouse event while the card shows and the mouse is free: forwarded to it; true when it was a click on the card,
    /// which the room must not see.
    bool handleWorldCardEvent(const SDL_Event& event);
    /// The card's button a click pressed since the last call.
    [[nodiscard]] std::optional<ui::CardButton> takeWorldCardClick();
    /// The card shows with its content cut by its maximum height; its cover's box in the view's pixels (x, y, width,
    /// height), if it shows one.
    [[nodiscard]] bool worldCardSqueezed() const;
    [[nodiscard]] std::optional<glm::vec4> worldCardCoverBox() const;
    /// Registers `texture` for the documents' image source "rv-texture:<key>" (ui::UiSystem::setTexture; 0: none).
    void setUiTexture(std::string_view key, unsigned texture, glm::ivec2 size);
    /// The media viewer and Informazioni (I3 step 14b; game_screens_panel.hpp) of `game`, from the world's card
    /// (`from`: kScreenFromCard; the quick catalog opens them itself): the catalog's row and stored media, `title` until
    /// the catalog answers. They take the UI's input while they show (panelOpen()); Back closes them (panelBack()).
    void openGameMedia(const catalog::GameId& game, std::string_view title, std::string_view from);
    void openGameInfo(const catalog::GameId& game, std::string_view title, std::string_view from);
    /// The other versions of a game's title Informazioni lists (the room's plan: VersionGroups; unset: none).
    void setVersionLookup(std::function<std::vector<catalog::GameId>(const catalog::GameId&)> lookup);
    /// The media viewer or Informazioni shows.
    [[nodiscard]] bool gameScreenOpen() const noexcept;
    /// Counts the catalog's changes that show other rows (its opening, a readiness resolved, a refresh): a reader of a
    /// game's row (the world's card) reads it again when it changed.
    [[nodiscard]] std::uint64_t catalogRevision() const noexcept;

    /// The catalog the UI shows (the generated room asks it for the room's snapshot, I3 step 10b).
    [[nodiscard]] AppCatalog& catalog() noexcept;

    /// One frame: follows a session whose panel shows and the window's return, applies the UI commands of
    /// `uiActions` (the frame's actions in the UI context; null: the product UI had no input when the frame began),
    /// shows the panel the launch wants and updates the view at `timeSeconds` (the simulated time).
    void update(const input::ActionState* uiActions, double deltaSeconds, double timeSeconds);
    /// The Back levels the application chose for a UI Back (BackAction::PanelBack, BackAction::CatalogBack). The
    /// panel's Back while a game runs is "Torna al gioco": in the focused window, the foreground goes to the game.
    void panelBack();
    void catalogBack();

    /// The host must idle: a launch waits for the frame boundary, or a game has the screen (LaunchFlow::hostIdle), or a
    /// lockstep run waits for the catalog worker.
    [[nodiscard]] bool wantsIdle() const noexcept;
    /// A launch is about to start or a game has the screen (LaunchFlow::pending, LaunchFlow::hostIdle).
    [[nodiscard]] bool launchActive() const noexcept;
    /// One wake of the host's idle wait: collects the catalog worker's answers, starts a pending launch (the window
    /// handed over before the process runs), follows the session, takes the window back once it ends.
    void onIdle();
    /// A frame was rendered.
    void frameRendered() noexcept;

    /// Draws the UI over the window's frame, and into a screenshot (scaled to fit), when something shows.
    void renderToWindow(gl::GLDevice& device);
    void renderTo(gl::GLDevice& device, const gl::Framebuffer& target);

    /// The end of the run (IAppLogic::onShutdown): a running launch is left running (LaunchFlow::quit), the catalog
    /// ends (its worker within a bound, the pending session writes tried), the catalog and the launches go into the run
    /// report.
    void finish();

private:
    struct Impl;

    explicit ProductUi(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
