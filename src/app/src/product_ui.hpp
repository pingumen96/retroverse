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
#include <rv/app/game_screens.hpp>
#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/app/library_data.hpp>
#include <rv/app/library_watch.hpp>
#include <rv/app/wizard_flow.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/ui/directory_model.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/ui/keyboard_model.hpp>
#include <rv/ui/regenerate_preview_model.hpp>
#include <rv/ui/settings_model.hpp>
#include <rv/ui/system_menu_model.hpp>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/app/edit_bar_ui.hpp>
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
    /// I5 step 9 (ADR-0016 §11): a room's launch was asked with its insertion (launchWorldGame() with `held`): the
    /// screens' decoders begin to stop and the sound to fade while the insertion runs, the hero screen showing the
    /// title card of `gameId`. `starting` follows before the hand-over, or `ended` when the launch never starts.
    std::function<void(const std::string& gameId)> preparing;
    /// A launch is about to start: called before the window's hand-over and the process, on the host's wake.
    std::function<void()> starting;
    /// I11 step 12 (ADR-0020 §1): the request about to start, adapted to its program (in VR, its arguments and
    /// append-file), after `starting`; its record says how.
    LaunchFlow::RequestAdapter adapting;
    /// The launch that started ended (the return), or failed to start: called once, when the flow says it returned;
    /// also for a prepared launch that never started (refused during the insertion).
    std::function<void()> ended;
};

/// What the application does for the menu and the settings screen (I7 step 1, ADR-0018 §1, §3), on the main thread.
struct MenuHooks {
    /// The rows the settings screen shows now (what is in force).
    std::function<std::vector<ui::SettingsRow>()> settingRows;
    /// A row changed, restored, or all of them restored: applied, in memory; the rows are read again after it.
    std::function<void(const ui::SettingsCommand&)> applySetting;
    /// The settings screen closed: what it changed goes to its file.
    std::function<void()> settingsClosed;
    /// Esci, confirmed.
    std::function<void()> quit;
    /// I7 step 3: what a press a capture read means for the row `key`, and the row that has it.
    std::function<std::pair<ui::SettingsCapture, std::string>(const std::string& key, const input::Binding& press)>
        judgeBinding;
};

/// What the application does for the wizard (I7 step 2, ADR-0018 §4), on the main thread. Its settings go through
/// MenuHooks.
struct WizardHooks {
    /// The ES-DE catalog that makes its database: EsdeAppCatalog with createDatabase (page 4).
    std::function<std::unique_ptr<AppCatalog>()> makeCatalog;
    /// The world starts in place of the test room (page 7).
    std::function<void()> startWorld;
    /// The world has shown what it could: Entra may be offered.
    std::function<bool()> worldReady;
    /// Esci.
    std::function<void()> quit;
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
class SystemLogos;

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
    /// step 14b: Back's PanelBack level closes them first), or the regeneration's preview (I4b step 6).
    [[nodiscard]] bool panelOpen() const noexcept;
    /// The product UI has the input (InputContext::Ui): the catalog, the directory, the menu or the panel is open.
    [[nodiscard]] bool hasInput() const noexcept {
        return catalogOpen() || panelOpen() || directoryOpen() || menuOpen();
    }

    /// The menu (I7 step 1, ADR-0018 §1: ui::SystemMenuModel on assets/ui/system_menu.rml through ui::SystemMenuScreen)
    /// shows, or a section of it: the UI's input, its Back level.
    [[nodiscard]] bool menuOpen() const noexcept;
    /// The first-run wizard (I7 step 2, ADR-0018 §4; app::WizardFlow, ui::WizardModel on assets/ui/wizard.rml through
    /// ui::WizardScreen) over the run from now on, `start` Fresh or Resume. It has the UI's input and the menu's Back level
    /// (menuOpen() is true while it shows, menuBack() is its Back).
    void startWizard(WizardFlow::Options options, WizardStart start, WizardHooks hooks);
    [[nodiscard]] bool wizardOpen() const noexcept;
    /// Opens it on Riprendi (`via`: "start" or "back", for the run report); its screen is made the first time (one that
    /// cannot be made is logged and it stays closed). The application decides when it may open.
    void openMenu(std::string_view via);
    /// Its Back level (BackAction::MenuBack): a section to the menu, the confirmation to the menu, the menu to the world.
    void menuBack();
    /// The application's side of the menu and of the settings screen (replacing any set before).
    void setMenuHooks(MenuHooks hooks);
    /// The catalog from now on, in place of the one held (the wizard's page 4): the old one ends, the list and the quick
    /// catalog's screen are made again over the new one's source.
    void replaceCatalog(std::unique_ptr<AppCatalog> catalog);
    /// The rows of the settings screen read again (MenuHooks::settingRows), when it shows.
    void refreshSettings();
    /// The UI's text scale from now on (UiView::setTextScale).
    void setTextScale(float scale);

    /// I7 step 3 (ADR-0018 §7): the bindings the hints name and the UI's keys, from now on.
    void setBindings(const input::ActionMap& bindings);
    /// The frame's raw input, what a capture reads, and the device used last, which the hints follow.
    void inputFrame(const input::RawInputFrame& raw, input::DeviceKind device);
    /// A capture of the settings screen waits for a press: the frame's actions act on nothing.
    [[nodiscard]] bool capturingBinding() const noexcept;
    /// `action`'s name on `device` (assets/ui/key_labels.json), "" when it has no binding there.
    [[nodiscard]] std::string bindingLabel(input::Action action, input::DeviceKind device) const;
    /// The same for a prompt, which must read: the other device's name when `device` has none.
    [[nodiscard]] std::string promptLabel(input::Action action, input::DeviceKind device) const;
    /// "Controller scollegato" shows, over whatever shows.
    void setControllerNotice(bool shown);

    /// The directory (I4 step 7b; ADR-0013 §10; docs/i4/step-7a.md's contract: ui::DirectoryModel on
    /// assets/ui/directory.rml through ui::DirectoryScreen): open (the UI's input, its Back level), its floors and rooms
    /// from the world (setDirectoryBuilding), its favourites and recent games from the catalog worker
    /// (AppCatalog::requestRecentGames, asked whenever the model asks), its card the quick catalog's (the catalog's row,
    /// readiness and blocker; the room key of setRoomPlacement()), Avvia on it launched here as the quick catalog's
    /// (its outcome back on the card), the world's commands (ChooseFloor, ChooseRoom, ShowInRoom) handed to the world
    /// (takeDirectoryCommands()).
    [[nodiscard]] bool directoryOpen() const noexcept;
    /// I4b step 6: the tab the directory shows (none while it is closed): "Modifiche" reads the regenerations' inputs
    /// ahead.
    [[nodiscard]] std::optional<ui::DirectoryTab> directoryTab() const noexcept;
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
    /// A directory command waits to be taken (the directory closed by it this frame).
    [[nodiscard]] bool directoryCommandsPending() const noexcept;
    /// `command` refused for now by the world (a ride in Focus, a plan not ready): the panel again where it was, with
    /// `notice` (ui::directory::kNotices) under its list.
    void refuseDirectory(const ui::DirectoryCommand& command, std::string_view notice);
    /// I4b step 6 (ADR-0015 §5, §6): the impact preview of a regeneration or an undo (ui::RegeneratePreviewModel on
    /// assets/ui/regenerate_preview.rml through ui::RegeneratePreviewScreen): opened on `kind` for `scope`, computing
    /// (the directory closed if it showed; its screen made the first time: one that cannot be made is logged and the
    /// preview stays closed), given the dry run's content or a failure, closed by its Cancel or by the world. It is a
    /// panel (panelOpen(): the UI's input, Back's PanelBack level). Its choices (Rigenera, Scarta e rigenera, Annulla
    /// la modifica, a cancel) are the world's (takeRegenerateCommands()).
    void openRegeneratePreview(ui::RegenerateKind kind, ui::RegenerateScopeTitle scope);
    void setRegeneratePreview(ui::RegeneratePreviewContent content);
    void failRegeneratePreview(std::string failure);
    void closeRegeneratePreview();
    [[nodiscard]] bool regeneratePreviewOpen() const noexcept;
    [[nodiscard]] const ui::RegeneratePreviewModel& regeneratePreview() const noexcept;
    [[nodiscard]] std::vector<ui::RegenerateCommandKind> takeRegenerateCommands();
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
    /// waits or runs. I5 step 9: `held`, the room's insertion into the console runs (ADR-0016 §11): the launch is
    /// resolved at once and LaunchHooks::preparing called, but a ready request waits for releaseWorldLaunch() before
    /// the flow takes it (the game-session panel, the hand-over); a refusal ends it at once as always.
    bool launchWorldGame(const catalog::GameId& game, std::string_view title, bool held = false);
    /// I5 step 9: the insertion ended: the room's held launch goes on (its decision now, or as it comes).
    void releaseWorldLaunch();
    /// I5 step 9's review: the room's launch asked by launchWorldGame() has not ended yet (decided, held for the
    /// insertion, waiting, running), whether or not launchActive() says so meanwhile.
    [[nodiscard]] bool worldLaunchInFlight() const noexcept;
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
    /// I4b step 3: the editor's bar from now on (nothing: none): shown over the scene, in screenshots too, while neither
    /// the quick catalog, the directory nor the game-session panel shows. It takes no input.
    void setEditBar(std::optional<EditBarContent> content);
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
    /// I11 step 3: the systems' logos the UI shows (null: none), and an RML <img> source for `system`'s logo once it is
    /// registered (empty before, or without one); their revision, a cache key for the UI's data.
    void setSystemLogos(const SystemLogos* logos) noexcept;
    [[nodiscard]] std::string systemLogoSource(std::string_view system) const;
    [[nodiscard]] std::uint64_t systemLogosRevision() const noexcept;
    /// The media viewer and Informazioni (I3 step 14b; game_screens_panel.hpp) of `game`, from the world's card
    /// (`from`: kScreenFromCard; the quick catalog opens them itself): the catalog's row and stored media, `title` until
    /// the catalog answers. They take the UI's input while they show (panelOpen()); Back closes them (panelBack()).
    void openGameMedia(const catalog::GameId& game, std::string_view title, std::string_view from);
    void openGameInfo(const catalog::GameId& game, std::string_view title, std::string_view from);
    /// The other versions of a game's title Informazioni lists (the room's plan: VersionGroups; unset: none).
    void setVersionLookup(std::function<std::vector<catalog::GameId>(const catalog::GameId&)> lookup);
    /// The media viewer or Informazioni shows.
    [[nodiscard]] bool gameScreenOpen() const noexcept;
    /// I5 step 8 (ADR-0016 §10): the media viewer shows (the world's screens pause under it, its clip alone sounds).
    [[nodiscard]] bool mediaViewerOpen() const noexcept;
    /// I5 step 8: what the viewer asks of the screens' service on this frame (GameScreensPanel::viewerRequest()), and
    /// the clip as the service left it after its frame (GameScreensPanel::setViewerVideo(): the page's state, its
    /// texture registered for the document while the videos page shows).
    [[nodiscard]] ViewerRequest viewerRequest() const;
    void setViewerVideo(const ViewerVideo& video);
    /// Counts the catalog's changes that show other rows (its opening, a readiness resolved, a refresh): a reader of a
    /// game's row (the world's card) reads it again when it changed.
    [[nodiscard]] std::uint64_t catalogRevision() const noexcept;

    /// The catalog the UI shows (the generated room asks it for the room's snapshot, I3 step 10b).
    [[nodiscard]] AppCatalog& catalog() noexcept;
    /// I7 step 7: the file watcher's refresh `request`, for the changes `watched` describes (the run report's refresh
    /// record says so); false, and nothing asked, when the catalog cannot take one now.
    bool requestWatcherRefresh(const WatchRefresh& watched, RefreshRequest request);
    /// I7 step 8: what the file watcher says of its roots, for the library screen (every frame while it watches).
    void setWatchStatus(WatchStatus status);
    /// The library screen shows (the watcher's modes are wanted).
    [[nodiscard]] bool libraryOpen() const noexcept;

    /// One frame: follows a session whose panel shows and the window's return, applies the UI commands of
    /// `uiActions` (the frame's actions in the UI context; null: the product UI had no input when the frame began),
    /// shows the panel the launch wants and updates the view at `timeSeconds` (the simulated time).
    void update(const input::ActionState* uiActions, double deltaSeconds, double timeSeconds);
    /// The Back levels the application chose for a UI Back (BackAction::PanelBack, BackAction::CatalogBack). The
    /// panel's Back while a game runs is "Torna al gioco": in the focused window, the foreground goes to the game.
    void panelBack();
    /// Closes the on-screen keyboard first (I7 step 5, ADR-0018 §8), when it shows.
    void catalogBack();

    /// The host must idle: a launch waits for the frame boundary, or a game has the screen (LaunchFlow::hostIdle), or a
    /// lockstep run waits for the catalog worker.
    [[nodiscard]] bool wantsIdle() const noexcept;
    /// A launch is about to start or a game has the screen (LaunchFlow::pending, LaunchFlow::hostIdle).
    [[nodiscard]] bool launchActive() const noexcept;
    /// The running launch's emulator window while it shows one (launcher::LaunchSession::window, I10 step 6), else 0.
    [[nodiscard]] std::uintptr_t launchWindow() const noexcept;
    /// I11 step 13 (ADR-0020 §5): the running game is in exclusive fullscreen (the runner's sign, or one noted here:
    /// the VR capture stayed black), which the session panel says.
    [[nodiscard]] bool launchExclusiveFullscreen() const noexcept;
    void noteExclusiveFullscreen();
    /// One wake of the host's idle wait: collects the catalog worker's answers, starts a pending launch (the window
    /// handed over before the process runs), follows the session, takes the window back once it ends.
    void onIdle();
    /// A frame was rendered.
    void frameRendered() noexcept;

    /// I10 step 5: a controller's ray on the VR mode's panel as the UI's mouse: moved to `pixel` (the view's pixels;
    /// none: off the panel, the mouse leaves), pressed and released (its trigger), `wheel` notches scrolled.
    void xrPointer(const std::optional<glm::vec2>& pixel, bool pressed, bool released, float wheel);
    /// Something of the UI shows (what renderTo() draws).
    [[nodiscard]] bool visible() const noexcept;

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
    const SystemLogos* m_logos = nullptr;
};

} // namespace rv::app
