#include "product_ui.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/library_refresh.hpp>
#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/window_handover.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/ui/game_card_screen.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/quick_catalog_screen.hpp>
#include <rv/ui/ui_command.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

#include <rv/app/directory_data.hpp>
#include <rv/app/world_card_ui.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/ui/directory_screen.hpp>
#include <rv/ui/regenerate_preview_screen.hpp>
#include <rv/ui/settings_screen.hpp>
#include <rv/ui/system_menu_screen.hpp>

#include "edit_bar_panel.hpp"
#include "elevator_prompt_panel.hpp"
#include "game_screens_panel.hpp"
#include "game_session_panel.hpp"
#include "world_label_panel.hpp"
#include "world_notice_panel.hpp"

namespace rv::app {
namespace {

using launcher::LaunchClock;

/// Developer builds read the assets from the source tree: the quick catalog's document reloads when saved.
#if defined(RV_DEV_ASSET_ROOT)
constexpr bool kWatchDocuments = true;
#else
constexpr bool kWatchDocuments = false;
#endif

/// A choice the panel's buttons made, applied once the frame's commands are done (never inside RmlUi's dispatch).
enum class PanelChoice : std::uint8_t { None, ReturnHome, Resume, Dismiss };

/// What the quick catalog shows of the catalog's refresh.
[[nodiscard]] ui::QuickCatalogRefresh shownRefresh(const RefreshState& state) {
    ui::QuickCatalogRefresh shown;
    shown.available = state.available;
    if (state.running) {
        shown.state = std::string(ui::quick_catalog::kRefreshRunning);
        const RefreshProgress progress = state.progress.value_or(RefreshProgress{});
        shown.phase = std::string(toString(progress.phase));
        shown.done = progress.done;
        shown.total = progress.total;
    } else if (state.last) {
        shown.state = std::string(toString(state.last->outcome));
        shown.added = state.last->added;
        shown.removed = state.last->removed;
        shown.changed = state.last->changed;
        shown.guarded = state.last->guarded;
    }
    return shown;
}

} // namespace

struct ProductUi::Impl {
    Impl(AppContext& context, std::unique_ptr<AppCatalog> catalog, std::function<void()> back,
         std::function<std::optional<SpawnPoint>()> pose)
        : context(context), back(std::move(back)), pose(std::move(pose)), catalog(std::move(catalog)) {}

    // Declared in the order they are created; destroyed in reverse: the screens before the model and the view, the
    // model before the catalog (its source), the view before RmlUi, the launch (its session) before the runner it
    // started from.
    AppContext& context;
    std::function<void()> back;
    std::function<std::optional<SpawnPoint>()> pose;
    std::unique_ptr<AppCatalog> catalog;
    platform_win::Win32ProcessRunner runner;
    LaunchFlow flow{runner};
    std::optional<platform::WindowHandover> handover;
    /// The window is coming back after a launch (WindowHandover::update every frame until it is).
    bool restoring = false;

    std::unique_ptr<ui::UiSystem> system;
    std::unique_ptr<ui::UiView> view;
    /// I10 step 5: a controller's ray is on the VR panel (the UI's mouse is there).
    bool xrPointing = false;
    std::unique_ptr<ui::QuickCatalogModel> model;
    std::unique_ptr<ui::QuickCatalogScreen> screen;
    std::unique_ptr<GameSessionPanel> panel;
    /// The world's notice (I3 step 10b) and what the application wants it to say; shown while neither the catalog nor
    /// the panel is.
    std::unique_ptr<WorldNoticePanel> notice;
    WorldNoticeContent wantedNotice;
    /// The hover's title label (I3 step 13a) and what the application wants it to show, likewise.
    std::unique_ptr<WorldLabelPanel> label;
    std::optional<WorldLabelContent> wantedLabel;
    /// The world's card beside the inspected item (I3 step 14a) and what the application wants it to show, likewise.
    std::unique_ptr<ui::GameCardScreen> card;
    std::optional<ui::GameCardContent> wantedCard;
    ui::CardButton wantedCardFocus = ui::CardButton::Play;
    /// The catalog's changes that show other rows (catalogRevision()).
    std::uint64_t catalogRevision = 0;
    /// The media viewer and Informazioni (I3 step 14b), over everything else, and the other versions' lookup.
    std::unique_ptr<GameScreensPanel> screens;
    std::function<std::vector<catalog::GameId>(const catalog::GameId&)> versionLookup;

    ui::NavInput nav;
    ui::UiCommandList commands;
    bool catalogOpen = false;
    std::uint64_t opened = 0;
    /// The screen asked to close (Back at its root), applied after the frame's commands.
    bool closeRequested = false;
    PanelChoice choice = PanelChoice::None;
    /// Runs with --fixed-dt wait between frames for what they asked of the catalog worker (lockstep), at most
    /// kLockstepWaitLimit at a time: past it a frame is rendered all the same (a warning logged), so that a
    /// frame-limited run never hangs on an answer that does not come.
    bool lockstep = false;
    static constexpr std::chrono::seconds kLockstepWaitLimit{10};
    std::optional<std::chrono::steady_clock::time_point> catalogWaitSince;
    /// What the quick catalog and the walker showed when Avvia asked for the launch being decided, until its record
    /// exists; a launch whose record waits for what they show after its end (the next frame).
    std::optional<CatalogView> before;
    bool afterPending = false;
    /// The launch whose session the flow follows: its token, its index among the run's launches.
    std::uint64_t sessionToken = 0;
    /// The catalog's refreshes (I3 step 13r): --refresh-library asks for one once the catalog opened; "Aggiorna
    /// libreria" sets refreshRequested, applied after the frame's commands. Their records; the last one waits for the
    /// list its commit made the model ask for (the selection restored) before it notes what the catalog shows after.
    bool refreshOnOpen = false;
    bool refreshAsked = false;
    bool refreshRequested = false;
    /// The record whose `after` waits for the list its commit made the model ask for.
    std::optional<std::size_t> refreshAfter;
    std::vector<RefreshRecord> refreshes;
    /// The application's hooks around launches (the hero CRT's decoder).
    LaunchHooks hooks;
    /// I3 step 15a: the token of the launch the room asked for (launchWorldGame) until it ended, and how it ended (until
    /// taken); the room's placement of a game for the catalog's card; a "Mostra nella stanza" not taken yet.
    std::optional<std::uint64_t> worldToken;
    std::optional<LaunchMessage> worldEnded;
    /// I5 step 9: the room's launch waits for its insertion (launchWorldGame() with `held`, until releaseWorldLaunch());
    /// its ready decision parked meanwhile; the screens prepared for it (LaunchHooks::preparing) and not yet started.
    bool worldHeld = false;
    std::optional<LaunchDecision> heldDecision;
    bool worldPrepared = false;
    std::function<std::string(std::string_view)> roomPlacement;
    std::optional<std::string> showInRoomAsked;
    /// The wall-clock times of the frames that showed games of the catalog (CatalogRecord::frames; at most
    /// kMaxFrameSamples), and when the last frame's update began (none after an idle wait, which is no frame time).
    std::vector<double> frameMs;
    std::optional<std::chrono::steady_clock::time_point> lastFrame;
    static constexpr std::size_t kMaxFrameSamples = 100000;

    /// I4 step 7b: the directory (its screen made when it first opens), the world's commands it gave and not taken
    /// yet, the games list and the card asked of the app (the requests answered last), a launch from its card and how
    /// it ended, its run report record; the elevator's prompt and the key it names.
    std::unique_ptr<ui::DirectoryScreen> directoryScreen;
    ui::DirectoryModel directory;
    std::vector<ui::DirectoryCommand> directoryCommands;
    std::uint64_t directoryGamesAsked = 0;
    std::uint64_t directoryCardServed = 0;
    std::uint64_t directoryCardRevision = 0;
    std::optional<std::uint64_t> directoryToken;
    std::string directoryLaunchGame;
    std::optional<LaunchMessage> directoryEnded;
    DirectoryRecord directoryRecord;
    std::unique_ptr<ElevatorPromptPanel> prompt;
    std::optional<std::string> wantedPrompt;
    bool wantedTotem = false;
    /// I4b step 3: the editor's bar and what the world wants it to show.
    std::unique_ptr<EditBarPanel> editBar;
    std::optional<EditBarContent> wantedEditBar;
    /// I4b step 6: the impact preview of a regeneration or an undo (its screen made when it first opens), the choices
    /// it gave and the world has not taken yet.
    std::unique_ptr<ui::RegeneratePreviewScreen> previewScreen;
    ui::RegeneratePreviewModel preview;
    std::vector<ui::RegenerateCommandKind> previewCommands;
    /// The preview's buttons as last seen and when they changed (the run's time): a click within
    /// kPreviewClickGuardSeconds of a change is the second click of a double-click meant for what showed before (I4b
    /// step 6's review: never "Scarta e rigenera" by a double-click on "Rigenera e scarta").
    std::vector<ui::RegenerateButton> previewButtonsSeen;
    double previewButtonsSince = 0.0;
    static constexpr double kPreviewClickGuardSeconds = 0.5;
    /// I7 step 1: the menu and the settings screen over it (their screens made when they first open), the application's
    /// side of them, their run report records.
    std::unique_ptr<ui::SystemMenuScreen> menuScreen;
    ui::SystemMenuModel menu;
    std::unique_ptr<ui::SettingsScreen> settingsScreen;
    ui::SettingsModel settingsModel;
    MenuHooks menuHooks;
    MenuRecord menuRecord;

    [[nodiscard]] bool visible() const noexcept {
        return catalogOpen || panel->visible() || (notice && notice->visible()) || (label && label->visible()) ||
               (card && card->visible()) || screensOpen() || directoryShown() || (prompt && prompt->visible()) ||
               (editBar && editBar->visible()) || previewShown() || menuShown() || settingsShown();
    }

    /// The menu shows (not while a section shows over it).
    [[nodiscard]] bool menuShown() const noexcept { return menuScreen && menu.isOpen() && !settingsModel.isOpen(); }
    /// The settings screen shows.
    [[nodiscard]] bool settingsShown() const noexcept { return settingsScreen && settingsModel.isOpen(); }

    void addMenuEvent(std::string event, std::string detail = {}) {
        if (menuRecord.events.size() >= kMaxInteractionEvents) {
            ++menuRecord.eventsDropped;
            return;
        }
        menuRecord.events.push_back(MenuEventRecord{context.frameIndex(), std::move(event), std::move(detail)});
    }

    void menuCommand(const std::optional<ui::SystemMenuCommand>& command) {
        if (!command) {
            return;
        }
        spdlog::info("Frame {}: the menu asks {}", context.frameIndex(), ui::toString(*command));
        switch (*command) {
        case ui::SystemMenuCommand::Close:
            addMenuEvent("closed");
            break;
        case ui::SystemMenuCommand::OpenSettings:
            openSettings();
            break;
        case ui::SystemMenuCommand::Quit:
            addMenuEvent("quit");
            menuRecord.quit = true;
            if (menuHooks.quit) {
                menuHooks.quit();
            }
            break;
        }
    }

    void openSettings() {
        if (!settingsScreen) {
            auto screen = ui::SettingsScreen::create(*view);
            if (!screen) {
                spdlog::error("The settings screen unavailable: {}", screen.error().toString());
                return;
            }
            settingsScreen = std::move(*screen);
        } else if (Rml::Element* root = view->context().GetRootElement()) {
            root->Focus(); // as the directory's: the focus the screen applies starts from the context's root
        }
        nav.reset();
        if (menuHooks.settingRows) {
            settingsModel.setRows(menuHooks.settingRows());
        }
        settingsModel.open();
        ++menuRecord.settingsOpened;
        addMenuEvent("settings");
        spdlog::info("Frame {}: the settings screen opened", context.frameIndex());
    }

    void settingsCommand(const std::optional<ui::SettingsCommand>& command) {
        if (!command) {
            return;
        }
        spdlog::info("Frame {}: the settings screen asks {} '{}'", context.frameIndex(), ui::toString(command->kind),
                     command->key);
        if (command->kind == ui::SettingsCommand::Kind::Close) {
            addMenuEvent("settings-closed");
            if (menuHooks.settingsClosed) {
                menuHooks.settingsClosed();
            }
            nav.reset();
            return;
        }
        if (menuHooks.applySetting) {
            menuHooks.applySetting(*command);
        }
        if (menuHooks.settingRows) {
            settingsModel.setRows(menuHooks.settingRows());
        }
    }

    void syncMenu() {
        if (menuScreen) {
            if (menuShown()) {
                menuScreen->show(menu);
            } else {
                menuScreen->hide();
            }
        }
        if (settingsScreen) {
            if (settingsModel.isOpen()) {
                settingsScreen->show(settingsModel);
            } else {
                settingsScreen->hide();
            }
        }
    }

    /// I4b step 6: the preview shows.
    [[nodiscard]] bool previewShown() const noexcept { return previewScreen && preview.isOpen(); }
    /// A choice of the preview's model handed to the world (its panel closes itself on Cancel).
    void previewCommand(const std::optional<ui::RegenerateCommandKind>& command) {
        if (!command) {
            return;
        }
        spdlog::info("Frame {}: the preview of {} asks {}", context.frameIndex(), ui::toString(preview.kind()),
                     ui::toString(*command));
        previewCommands.push_back(*command);
    }
    void syncPreview() {
        if (!previewScreen) {
            return;
        }
        if (preview.isOpen()) {
            previewScreen->show(preview);
        } else {
            previewScreen->hide();
        }
    }

    /// The media viewer or Informazioni shows (I3 step 14b).
    [[nodiscard]] bool screensOpen() const noexcept { return screens && screens->visible(); }
    /// The directory shows (I4 step 7b).
    [[nodiscard]] bool directoryShown() const noexcept { return directoryScreen && directory.isOpen(); }
    /// Something over the world that hides the world's own labels: the catalog, the panel, the screens, the directory.
    [[nodiscard]] bool overWorld() const noexcept {
        return catalogOpen || panel->visible() || screensOpen() || directoryShown() || previewShown() || menuShown() ||
               settingsShown();
    }

    /// The world's notice shows what the application wants while neither the catalog nor the panel shows.
    void syncNotice() {
        if (!notice) {
            return;
        }
        if (wantedNotice.notice.empty() || overWorld()) {
            notice->hide();
        } else {
            notice->show(wantedNotice);
        }
    }

    /// The hover's label likewise, in the view's pixels.
    void syncLabel() {
        if (!label) {
            return;
        }
        if (!wantedLabel || overWorld()) {
            label->hide();
        } else {
            label->show(*wantedLabel, context.framebufferSize());
        }
    }

    /// The world's card likewise.
    void syncCard() {
        if (!card) {
            return;
        }
        if (!wantedCard || overWorld()) {
            card->hide();
        } else {
            card->show(*wantedCard, wantedCardFocus);
        }
    }

    /// I4b step 3: the editor's bar likewise.
    void syncEditBar() {
        if (!editBar) {
            return;
        }
        if (!wantedEditBar || overWorld()) {
            editBar->hide();
        } else {
            editBar->show(*wantedEditBar);
        }
    }

    /// The elevator's prompt likewise (I4 step 7b).
    void syncPrompt() {
        if (!prompt) {
            return;
        }
        if (!wantedPrompt || overWorld()) {
            prompt->hide();
        } else {
            prompt->show(*wantedPrompt, wantedTotem);
        }
    }

    // --- The directory (I4 step 7b) ------------------------------------------------------------------------------

    void addDirectoryEvent(std::string event, std::string id = {}, std::string detail = {}) {
        if (directoryRecord.events.size() >= kMaxInteractionEvents) {
            ++directoryRecord.eventsDropped;
            return;
        }
        directoryRecord.events.push_back(
            DirectoryEventRecord{context.frameIndex(), std::move(event), std::move(id), std::move(detail)});
    }

    /// A command of the directory's model: Avvia launched here, Close recorded, the world's handed over.
    void directoryCommand(const std::optional<ui::DirectoryCommand>& command) {
        if (!command) {
            return;
        }
        spdlog::info("Frame {}: the directory asks {} '{}'", context.frameIndex(), ui::toString(command->kind),
                     command->id);
        addDirectoryEvent("command", std::string(ui::toString(command->kind)), command->id);
        switch (command->kind) {
        case ui::DirectoryCommandKind::Launch:
            launchFromDirectory(command->id);
            break;
        case ui::DirectoryCommandKind::Close:
            addDirectoryEvent("closed");
            break;
        case ui::DirectoryCommandKind::ChooseFloor:
        case ui::DirectoryCommandKind::ChooseRoom:
        case ui::DirectoryCommandKind::ShowInRoom:
        case ui::DirectoryCommandKind::EditRoom:
        case ui::DirectoryCommandKind::RegenerateRoom:  // I4b step 6
        case ui::DirectoryCommandKind::RegenerateFloor: // I4b step 6
        case ui::DirectoryCommandKind::RegenerateWorld: // I4b step 6
        case ui::DirectoryCommandKind::Undo:            // I4b step 6
            directoryCommands.push_back(*command);
            break;
        }
    }

    /// The title the games list shows for `gameId` ("" when it does not list it).
    [[nodiscard]] std::string directoryTitleOf(std::string_view gameId) const {
        for (const ui::DirectoryGame& game : directory.games()) {
            if (game.gameId == gameId) {
                return game.title;
            }
        }
        return {};
    }

    /// Avvia on the directory's card: the quick catalog's launch of `gameId` (its outcome back on the card).
    void launchFromDirectory(const std::string& gameId) {
        ui::CatalogEntry entry;
        entry.id = gameId;
        entry.title = directoryTitleOf(gameId);
        directoryEnded.reset();
        directoryLaunchGame = gameId;
        if (const std::optional<std::uint64_t> token = launch(entry)) {
            directoryToken = token;
            spdlog::info("Frame {}: Avvia in the directory on '{}' ({}): launch {} asked", context.frameIndex(),
                         entry.title, gameId, *token);
            return;
        }
        // Another launch is decided, waits or runs: nothing starts, the card says so.
        directoryEnded = LaunchMessage{std::string(launch_outcome::kNotReady), "busy",
                                       "another launch is being decided or runs"};
        addDirectoryEvent("launch-ended", gameId, directoryEnded->code);
        fillDirectoryCard();
    }

    /// The card of the game the directory shows: the catalog's row (readiness, blocker, details), where the game stands
    /// in the building (the room key), the outcome of its last launch from the card.
    void fillDirectoryCard() {
        if (directory.view() != ui::DirectoryView::Card || directory.cardGameId().empty()) {
            return;
        }
        const std::string& gameId = directory.cardGameId();
        const auto game = catalog::GameId::parse(gameId);
        if (!game) {
            return;
        }
        const LaunchMessage outcome = directoryEnded && directoryLaunchGame == gameId ? *directoryEnded : LaunchMessage{};
        ui::GameCardContent content =
            cardContentOf(*game, directoryTitleOf(gameId), false, catalog->gameEntry(*game), 0, false,
                          outcome.code.empty() ? InteractionNotice::None : InteractionNotice::LaunchOutcome, outcome);
        std::string room = roomPlacement ? roomPlacement(gameId) : std::string();
        if (directoryRecord.cardGame != gameId || directoryRecord.cardRoom != room) {
            addDirectoryEvent("card", gameId, room);
        }
        directoryRecord.cardGame = gameId;
        directoryRecord.cardRoom = room;
        directoryRecord.cardOutcome = content.outcome;
        directory.setCard(std::move(content), std::move(room));
        directoryCardRevision = catalogRevision;
    }

    /// The directory's requests of the app: its games list (the catalog worker's favourites and recent games), its
    /// card; and the screen shown or hidden as the model is.
    void syncDirectory() {
        if (directory.gamesRequest() != directoryGamesAsked) {
            directoryGamesAsked = directory.gamesRequest();
            if (catalog->requestRecentGames(catalog::kDefaultRecentGames)) {
                ++directoryRecord.gamesRequests;
                directory.setGamesState(ui::DirectoryGamesState::Loading);
            } else {
                directory.setGamesState(ui::DirectoryGamesState::Failed);
            }
        }
        if (std::optional<Expected<std::vector<catalog::RecentGame>>> answer = catalog->takeRecentGames()) {
            if (*answer) {
                std::vector<ui::DirectoryGame> games;
                directoryRecord.games.clear();
                for (const catalog::RecentGame& recent : **answer) {
                    ui::DirectoryGame game = directoryGameOf(recent, machineLocalDate);
                    directoryRecord.games.push_back(DirectoryGameRecord{game.gameId, game.title, game.system,
                                                                        game.favorite, dateText(game.played)});
                    games.push_back(std::move(game));
                }
                addDirectoryEvent("games", "ready", std::to_string(games.size()));
                spdlog::info("Frame {}: the directory lists {} favourite or recent game(s)", context.frameIndex(),
                             games.size());
                directory.setGames(std::move(games));
            } else {
                spdlog::warn("The directory's favourites and recent games cannot be read: {}",
                             answer->error().toString());
                addDirectoryEvent("games", "failed");
                directory.setGamesState(ui::DirectoryGamesState::Failed);
            }
        }
        if (directory.view() == ui::DirectoryView::Card &&
            (directory.cardRequest() != directoryCardServed || directoryCardRevision != catalogRevision)) {
            directoryCardServed = directory.cardRequest();
            fillDirectoryCard();
        }
        if (!directoryScreen) {
            return;
        }
        if (directory.isOpen()) {
            directoryScreen->show(directory);
        } else {
            directoryScreen->hide();
        }
    }

    /// What the quick catalog and the walker show now.
    [[nodiscard]] CatalogView currentView() const {
        CatalogView shown;
        if (const ui::CatalogEntry* selected = model->selected()) {
            shown.selectedId = selected->id;
            shown.readiness = selected->readiness;
            shown.blocker = selected->blocker;
        }
        shown.selectedPosition = model->selectedPosition();
        shown.search = model->search();
        shown.filter = model->activeFilter();
        if (pose) {
            shown.camera = pose();
        }
        return shown;
    }

    /// Avvia on `entry`: the catalog decides, at once (the mock) or on its worker; applyDecision() acts on it.
    /// The token of the launch asked (nothing while one is decided, waits or runs: nothing asked).
    std::optional<std::uint64_t> launch(const ui::CatalogEntry& entry) {
        if (flow.pending() || flow.sessionActive() || catalog->deciding()) {
            return std::nullopt;
        }
        const std::uint64_t token = flow.records().size();
        if (!catalog->requestLaunch(token, entry)) {
            return std::nullopt;
        }
        before = currentView();
        return token;
    }

    /// The room's launch `token` ended with `message` (I3 step 15a); or the directory's (I4 step 7b): its card tells it.
    void worldLaunchEnded(std::uint64_t token, const LaunchMessage& message) {
        if (worldToken == token) {
            worldEnded = message;
            worldToken.reset();
            worldHeld = false;
            heldDecision.reset();
            // A launch prepared for its insertion that never started: the screens play again (I5 step 9).
            if (std::exchange(worldPrepared, false) && hooks.ended) {
                hooks.ended();
            }
        }
        if (directoryToken == token) {
            directoryToken.reset();
            directoryEnded = message;
            addDirectoryEvent("launch-ended", directoryLaunchGame, message.code);
            spdlog::info("Frame {}: the directory's launch of {} ended ({})", context.frameIndex(),
                         directoryLaunchGame, message.code);
            fillDirectoryCard();
        }
    }

    /// The card's room key of `entry` (setRoomPlacement()).
    void applyRoomPlacement() {
        if (!screen) {
            return;
        }
        if (!roomPlacement) {
            screen->setRoomPlacement({});
            return;
        }
        screen->setRoomPlacement(
            [placement = roomPlacement](const ui::CatalogEntry& entry) { return placement(entry.id); });
    }

    /// The media viewer of `gameId` (I3 step 14b): its catalog row (`entry` when the caller has it, else read now) and
    /// stored media's pages; `title` until the catalog answers.
    void openMedia(std::string_view gameId, std::string_view title, std::optional<ui::CatalogEntry> entry,
                   std::string_view from) {
        if (!screens) {
            return;
        }
        GameScreensPanel::MediaRequest request;
        request.gameId = std::string(gameId);
        request.title = std::string(title);
        request.from = std::string(from);
        if (const auto game = catalog::GameId::parse(gameId)) {
            if (!entry) {
                entry = catalog->gameEntry(*game);
            }
            if (const std::optional<catalog_db::GameDetails> details = catalog->gameDetails(*game)) {
                request.pages = viewerPagesOf(*details);
            }
        }
        if (entry) {
            if (!entry->title.empty()) {
                request.title = entry->title;
            }
            request.platform = entry->platform;
        }
        screens->openMedia(std::move(request), context.frameIndex());
    }

    /// Informazioni of `gameId` (I3 step 14b), likewise, with the other versions the lookup knows.
    void openInfo(std::string_view gameId, std::string_view title, std::optional<ui::CatalogEntry> entry,
                  std::string_view from) {
        if (!screens) {
            return;
        }
        std::vector<ui::OtherVersion> versions;
        if (const auto game = catalog::GameId::parse(gameId)) {
            if (!entry) {
                entry = catalog->gameEntry(*game);
            }
            if (versionLookup) {
                for (const catalog::GameId& other : versionLookup(*game)) {
                    // Their titles and regions for those listed; the rest are counted.
                    versions.push_back(versions.size() < ui::game_info::kMaxVersions
                                           ? otherVersionOf(other, catalog->gameDetails(other))
                                           : ui::OtherVersion{other.toString(), {}, {}});
                }
            }
        }
        screens->openInfo(gameInfoOf(gameId, title, entry, std::move(versions)), std::string(from),
                          context.frameIndex());
    }

    /// What the screens asked of the product UI (the other screen of the same game).
    void switchScreen(GameScreensPanel::Switch asked) {
        if (!screens || asked == GameScreensPanel::Switch::None) {
            return;
        }
        const std::string game = screens->gameId();
        const std::string from = screens->from();
        if (asked == GameScreensPanel::Switch::Media) {
            openMedia(game, {}, std::nullopt, from);
        } else {
            openInfo(game, {}, std::nullopt, from);
        }
    }

    /// A decision the catalog made: the launch requested, or refused with its message (and its play session).
    void applyDecision() {
        std::optional<LaunchDecision> decision = catalog->takeDecision();
        if (!decision) {
            return;
        }
        // I5 step 9: the room's ready launch waits for its insertion (a refusal is told at once).
        if (decision->request && worldHeld && worldToken == decision->token) {
            spdlog::info("Frame {}: launch {} is ready; it waits for the insertion into the console",
                         context.frameIndex(), decision->token);
            heldDecision = std::move(decision);
            return;
        }
        applyDecision(std::move(*decision));
    }

    /// `decision` applied: the launch requested of the flow, or refused with its message (and its play session).
    void applyDecision(LaunchDecision decision) {
        const std::uint64_t frame = context.frameIndex();
        if (decision.request) {
            sessionToken = decision.token;
            flow.request(std::move(*decision.request), std::move(decision.info), frame);
        } else {
            const LaunchMessage refusal = decision.refusal;
            if (flow.refuse(std::move(decision.refusal), std::move(decision.info), frame)) {
                catalog->sessionRefused(decision.token);
                afterPending = true;
            }
            worldLaunchEnded(decision.token, refusal);
        }
        if (before) {
            flow.noteBefore(std::move(*before));
            before.reset();
        }
    }

    /// What the catalog worker answered: the catalog opened (the platforms and the list read again), rows that show
    /// other data now (read again), a refresh that ended (after its commit the platforms and the list read again, the
    /// selection restored by GameId), the list of a query (collected by the model, which the screen shows after its
    /// update). Polled here rather than only by the screen, which does not exist before the catalog first opens.
    void syncCatalog() {
        const CatalogChanges changes = catalog->poll();
        if (changes.opened || changes.rowsChanged || changes.refreshed) {
            ++catalogRevision;
        }
        if (changes.opened) {
            model->refresh();
        }
        if (changes.refreshEnded) {
            refreshEnded(changes.refreshed);
        }
        if (changes.rowsChanged && !changes.refreshed) {
            model->reread();
        }
        model->poll();
        // --refresh-library: once the catalog opened, with its first list applied (what the refresh's record notes as
        // shown before it).
        if (changes.opened && refreshOnOpen && !std::exchange(refreshAsked, true)) {
            requestRefresh(RefreshTrigger::CommandLine);
        }
        if (refreshAfter && !model->pending()) {
            RefreshRecord& record = refreshes.at(*std::exchange(refreshAfter, std::nullopt));
            record.after = currentView();
            spdlog::info("Frame {}: the catalog's list after the refresh: {} game(s), the selection {}",
                         context.frameIndex(), model->count(),
                         record.before && record.after->selectedId == record.before->selectedId ? "kept" : "moved");
        }
    }

    /// Starts a refresh of the catalog (the import on its worker), its phase opened for the frame times.
    void requestRefresh(RefreshTrigger trigger) {
        const std::uint64_t frame = context.frameIndex();
        if (!catalog->requestRefresh()) {
            if (trigger == RefreshTrigger::CommandLine) {
                RefreshRecord record;
                record.trigger = std::string(toString(trigger));
                record.outcome = "unavailable";
                record.error = catalog->supportsRefresh() ? std::string("the catalog is not open")
                                                          : std::string("this catalog has nothing to refresh");
                record.requestFrame = frame;
                spdlog::warn("--refresh-library: no refresh: {}", record.error);
                refreshes.push_back(std::move(record));
            } else {
                spdlog::info("Frame {}: Aggiorna libreria: no refresh now (one runs, or the catalog is not open)",
                             frame);
            }
            return;
        }
        RefreshRecord record;
        record.trigger = std::string(toString(trigger));
        record.outcome = std::string(ui::quick_catalog::kRefreshRunning);
        record.requestFrame = frame;
        record.before = currentView();
        refreshes.push_back(std::move(record));
        context.frameTiming().openPhase(kPhaseRefresh);
        spdlog::info("Frame {}: catalog refresh asked ({}): phase '{}' opened", frame, toString(trigger),
                     kPhaseRefresh);
    }

    /// The refresh ended: its record, the phase closed; after a commit the list runs again (the selection kept).
    void refreshEnded(bool refreshed) {
        const RefreshState state = catalog->refreshState();
        if (context.frameTiming().closePhase(kPhaseRefresh)) {
            spdlog::info("Frame {}: catalog refresh ended ({}): phase '{}' closed", context.frameIndex(),
                         state.last ? toString(state.last->outcome) : std::string_view("unknown"), kPhaseRefresh);
        }
        if (!refreshes.empty() && refreshes.back().outcome == ui::quick_catalog::kRefreshRunning) {
            RefreshRecord& record = refreshes.back();
            if (state.last) {
                recordRefresh(record, *state.last, state.lastLists);
            }
            record.endFrame = context.frameIndex();
            if (!refreshed) {
                record.after = currentView();
            }
        }
        if (refreshed) {
            model->refresh();
            refreshAfter = refreshes.empty() ? std::nullopt : std::optional<std::size_t>(refreshes.size() - 1);
        }
    }

    /// The lockstep run waits for the catalog worker (within kLockstepWaitLimit).
    [[nodiscard]] bool waitsForCatalog() const noexcept {
        // The media viewer's pictures too (I3 step 14b), so that scripted runs see them on the same frames.
        if (!lockstep || (!catalog->busy() && !(screens && screens->loading()))) {
            return false;
        }
        return !catalogWaitSince || std::chrono::steady_clock::now() - *catalogWaitSince < kLockstepWaitLimit;
    }

    /// The window's hand-over for the launch about to start (visible windows only).
    void beginHandover() {
        handover.reset();
        restoring = false;
        if (context.options().hidden) {
            return;
        }
        handover.emplace(context.window().handle(), platform::HandoverPolicy::Keep);
        handover->prepare();
    }

    /// After a session ended: the window comes back (restored, raised; update() follows it frame by frame), the UI's
    /// held directions are forgotten (the input of the wait was dropped). Not while processes of the game's tree may
    /// still run (LaunchFlow::returnTakesForeground): the window stays where it is, behind the game or minimized, and
    /// its message waits for the user.
    void afterReturn() {
        if (!flow.takeReturned()) {
            return;
        }
        if (hooks.ended) {
            hooks.ended();
        }
        worldLaunchEnded(sessionToken, flow.message());
        nav.reset();
        afterPending = true;
        if (!handover) {
            return;
        }
        if (flow.returnTakesForeground()) {
            handover->restore();
            restoring = true;
        } else {
            spdlog::info("Window left where it is after the launch: processes of the game may still run");
            handover.reset();
            restoring = false;
        }
    }

    /// "Torna al gioco" in the focused window (LaunchFlow::backToGame): the window is minimized, so that Windows
    /// activates the next window, the game the user came from; WindowHandover::restore() takes it back at the return,
    /// the user from the taskbar before that (the question comes back then). Visible windows only (the hand-over).
    void handForegroundToGame() {
        if (!handover) {
            return;
        }
        spdlog::info("Back to the game: Retroverse's window is minimized so that the game takes the foreground");
        if (!SDL_MinimizeWindow(context.window().handle())) {
            spdlog::warn("Back to the game: the window could not be minimized: {}", SDL_GetError());
        }
    }

    void followHandover() {
        if (!handover || !restoring) {
            return;
        }
        if (handover->update()) {
            if (const auto foreground = handover->report().foreground) {
                flow.noteForeground(*foreground);
            }
            spdlog::info("Window back after the launch: restored, in fullscreen as before and in the foreground");
            handover.reset();
            restoring = false;
        }
    }

    /// StillRunning's text names the window timeout.
    [[nodiscard]] int timeoutSeconds() const {
        const launcher::LaunchSession* session = flow.session();
        if (session == nullptr) {
            return 0;
        }
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(session->request().timeouts.windowTimeout).count());
    }

    /// Shows the panel the launch wants, when it is not the one shown.
    void syncPanel() {
        const SessionPanel wanted = flow.panel();
        if (wanted != panel->shown()) {
            panel->show(wanted, flow.info().title, flow.message(), timeoutSeconds());
        }
    }

    void applyChoice() {
        const PanelChoice made = std::exchange(choice, PanelChoice::None);
        switch (made) {
        case PanelChoice::None:
            break;
        case PanelChoice::ReturnHome:
            flow.returnToRetroverse(LaunchClock::now(), context.frameIndex());
            break;
        case PanelChoice::Resume:
            if (flow.backToGame(LaunchClock::now())) {
                handForegroundToGame();
            }
            break;
        case PanelChoice::Dismiss:
            flow.dismissOutcome();
            break;
        }
    }
};

ProductUi::ProductUi(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

ProductUi::~ProductUi() = default;

Expected<std::unique_ptr<ProductUi>> ProductUi::create(AppContext& context, std::unique_ptr<AppCatalog> catalog,
                                                       const input::ActionMap& bindings, std::function<void()> back,
                                                       std::function<std::optional<SpawnPoint>()> pose) {
    const core::Paths& paths = context.paths();
    auto impl = std::make_unique<Impl>(context, std::move(catalog), std::move(back), std::move(pose));
    Impl& s = *impl;
    s.lockstep = context.options().fixedDeltaSeconds.has_value();
    s.refreshOnOpen = context.options().refreshLibrary;
    // The focus the window starts with (its later changes come as events); a hidden window counts as focused: the
    // input script stands for the user in front of it.
    const bool focused = context.options().hidden ||
                         (SDL_GetWindowFlags(context.window().handle()) & SDL_WINDOW_INPUT_FOCUS) != 0;
    s.flow.focusChanged(focused, LaunchClock::now());
    if (const std::optional<double> limit = context.options().launchWaitLimitSeconds) {
        s.flow.setWaitLimit(std::chrono::milliseconds(static_cast<std::int64_t>(*limit * 1000.0)));
    }
    Impl* state = impl.get();
    LaunchFlowEvents flowEvents;
    flowEvents.started = [state](const launcher::LaunchSession& session) {
        state->catalog->sessionStarted(state->sessionToken, session);
    };
    flowEvents.ended = [state](const launcher::LaunchSession& session, LaunchEndCause cause, bool stoppedAwaiting) {
        state->catalog->sessionEnded(state->sessionToken, session, cause, stoppedAwaiting);
    };
    s.flow.setEvents(std::move(flowEvents));

    auto system = ui::UiSystem::create(context.window(), ui::UiSystemConfig{paths.assetRoot()});
    if (!system) {
        return propagate(system.error());
    }
    s.system = std::move(*system);
    auto view = ui::UiView::create(*s.system, "window", context.framebufferSize(), context.window().displayScale());
    if (!view) {
        return propagate(view.error());
    }
    s.view = std::move(*view);
    s.view->setCommandKeys(ui::commandKeys(bindings));

    s.model = std::make_unique<ui::QuickCatalogModel>(s.catalog->source());
    // The catalog's screen is created when it first opens (openCatalog), as a screen shown from the start: created
    // hidden, its pool of rows would never be laid out before it shows.

    GameSessionPanelEvents panelEvents;
    panelEvents.returnHome = [state] { state->choice = PanelChoice::ReturnHome; };
    panelEvents.resume = [state] { state->choice = PanelChoice::Resume; };
    panelEvents.dismiss = [state] { state->choice = PanelChoice::Dismiss; };
    auto panel = GameSessionPanel::create(*s.view, std::move(panelEvents));
    if (!panel) {
        return propagate(panel.error());
    }
    s.panel = std::move(*panel);
    if (auto notice = WorldNoticePanel::create(*s.view)) {
        s.notice = std::move(*notice);
    } else {
        // The world goes on without its notice (style §21: a missing asset never stops the app).
        spdlog::error("World notice unavailable: {}", notice.error().toString());
    }
    if (auto label = WorldLabelPanel::create(*s.view)) {
        s.label = std::move(*label);
    } else {
        spdlog::error("Hover label unavailable: {}", label.error().toString());
    }
    if (auto prompt = ElevatorPromptPanel::create(*s.view)) {
        s.prompt = std::move(*prompt);
    } else {
        spdlog::error("Elevator prompt unavailable: {}", prompt.error().toString());
    }
    if (auto card = ui::GameCardScreen::create(*s.view)) {
        s.card = std::move(*card);
    } else {
        spdlog::error("World card unavailable: {}", card.error().toString());
    }
    if (auto editBar = EditBarPanel::create(*s.view)) {
        s.editBar = std::move(*editBar);
    } else {
        spdlog::error("The editor's bar unavailable: {}", editBar.error().toString());
    }
    // The media viewer and Informazioni (I3 step 14b), loaded after the other documents: over them when shown.
    if (auto screens = GameScreensPanel::create(*s.view, *s.system, paths.configDir() / esde::kLibraryFileName,
                                                context.wakeCallback())) {
        s.screens = std::move(*screens);
    } else {
        spdlog::error("Media viewer and Informazioni unavailable: {}", screens.error().toString());
    }
    if (s.refreshOnOpen && !s.catalog->supportsRefresh()) {
        // The mock catalog: nothing to import (recorded as such).
        s.refreshAsked = true;
        s.requestRefresh(RefreshTrigger::CommandLine);
    }
    spdlog::info("Quick catalog ready{}; QuickCatalog (Tab, gamepad Y) opens it",
                 s.lockstep ? " (lockstep: frames wait for the catalog worker's answers)" : "");
    return std::unique_ptr<ProductUi>(new ProductUi(std::move(impl)));
}

bool ProductUi::catalogOpen() const noexcept {
    return m_impl->catalogOpen;
}

bool ProductUi::panelOpen() const noexcept {
    // I4b step 6: the regeneration's preview is a panel too (Back's PanelBack level leaves it first).
    return m_impl->panel->visible() || m_impl->flow.panel() != SessionPanel::None || m_impl->screensOpen() ||
           m_impl->preview.isOpen();
}

void ProductUi::openRegeneratePreview(ui::RegenerateKind kind, ui::RegenerateScopeTitle scope) {
    Impl& s = *m_impl;
    if (!s.previewScreen) {
        auto screen = ui::RegeneratePreviewScreen::create(*s.view);
        if (!screen) {
            spdlog::error("The regeneration's preview unavailable: {}", screen.error().toString());
            return;
        }
        s.previewScreen = std::move(*screen);
    } else if (Rml::Element* root = s.view->context().GetRootElement()) {
        // As the directory's: the focus the screen applies starts from the context's root.
        root->Focus();
    }
    if (s.directory.isOpen()) {
        s.directory.close();
    }
    // A stick or direction still held must not move in the panel that just opened.
    s.nav.reset();
    s.previewCommands.clear();
    s.preview.open(kind, std::move(scope));
    spdlog::info("Frame {}: the preview of {} opened", s.context.frameIndex(), ui::toString(kind));
}

void ProductUi::setRegeneratePreview(ui::RegeneratePreviewContent content) {
    m_impl->preview.setContent(std::move(content));
}

void ProductUi::failRegeneratePreview(std::string failure) {
    m_impl->preview.setFailed(std::move(failure));
}

void ProductUi::closeRegeneratePreview() {
    m_impl->preview.close();
}

bool ProductUi::regeneratePreviewOpen() const noexcept {
    return m_impl->preview.isOpen();
}

const ui::RegeneratePreviewModel& ProductUi::regeneratePreview() const noexcept {
    return m_impl->preview;
}

std::vector<ui::RegenerateCommandKind> ProductUi::takeRegenerateCommands() {
    return std::exchange(m_impl->previewCommands, {});
}

void ProductUi::openCatalog() {
    Impl& s = *m_impl;
    if (s.catalogOpen) {
        return;
    }
    if (!s.screen) {
        // The first time: the screen is made shown, as a screen shown from the start (the spike's), with the focus on
        // the selected game.
        ui::QuickCatalogEvents events;
        Impl* state = &s;
        events.launch = [state](const ui::CatalogEntry& entry) { state->launch(entry); };
        // Vedi media and Informazioni open the same screens as the world's card (I3 step 14b).
        events.media = [state](const ui::CatalogEntry& entry) {
            state->openMedia(entry.id, entry.title, entry, kScreenFromCatalog);
        };
        events.info = [state](const ui::CatalogEntry& entry, bool /*shown*/) {
            state->openInfo(entry.id, entry.title, entry, kScreenFromCatalog);
        };
        events.close = [state] { state->closeRequested = true; };
        events.refresh = [state] { state->refreshRequested = true; };
        events.showInRoom = [state](const ui::CatalogEntry& entry) {
            spdlog::info("Frame {}: Mostra nella stanza on '{}' ({}): the catalog closes", state->context.frameIndex(),
                         entry.title, entry.id);
            state->showInRoomAsked = entry.id;
            state->closeRequested = true;
        };
        auto screen = ui::QuickCatalogScreen::create(*s.view, *s.model, std::move(events), kWatchDocuments);
        if (!screen) {
            spdlog::error("Quick catalog unavailable: {}", screen.error().toString());
            return;
        }
        s.screen = std::move(*screen);
        s.applyRoomPlacement();
        s.screen->setNotice(s.catalog->notice());
    } else {
        // QuickCatalogScreen::show() learns the focus it restores from RmlUi's focus event, which RmlUi sends only when
        // the focus moves; hide() left RmlUi's focus on the catalog's element, so the context's root takes it first
        // (else the screen would show the focus without knowing it, and Avvia on the list would do nothing).
        if (Rml::Element* root = s.view->context().GetRootElement()) {
            root->Focus();
        }
        s.screen->show();
    }
    // A stick or direction still held from walking must not start scrolling the catalog that just opened.
    s.nav.reset();
    s.catalogOpen = true;
    ++s.opened;
    spdlog::info("Quick catalog opened ({} game(s){})", s.model->count(),
                 s.catalog->notice().empty() ? std::string() : ", " + std::string(s.catalog->notice()));
}

void ProductUi::closeCatalog() {
    Impl& s = *m_impl;
    if (!s.catalogOpen || panelOpen()) {
        return;
    }
    if (s.screen) {
        s.screen->hide();
    }
    s.catalogOpen = false;
    spdlog::info("Quick catalog closed");
}

bool ProductUi::directoryOpen() const noexcept {
    return m_impl->directory.isOpen();
}

std::optional<ui::DirectoryTab> ProductUi::directoryTab() const noexcept {
    if (!m_impl->directory.isOpen()) {
        return std::nullopt;
    }
    return m_impl->directory.tab();
}

void ProductUi::openDirectory(ui::DirectoryTab tab, std::string_view via) {
    Impl& s = *m_impl;
    if (s.directory.isOpen() || s.catalogOpen) {
        return;
    }
    if (!s.directoryScreen) {
        auto screen = ui::DirectoryScreen::create(*s.view);
        if (!screen) {
            spdlog::error("Directory unavailable: {}", screen.error().toString());
            return;
        }
        s.directoryScreen = std::move(*screen);
    } else if (Rml::Element* root = s.view->context().GetRootElement()) {
        // As the quick catalog's: the focus the screen applies starts from the context's root.
        root->Focus();
    }
    s.directoryRecord.bound = true;
    // A stick or direction still held from walking must not move in the panel that just opened.
    s.nav.reset();
    s.directory.open(tab);
    ++s.directoryRecord.opened;
    s.addDirectoryEvent("open", std::string(ui::toString(tab)), std::string(via));
    spdlog::info("Frame {}: the directory opened on {} ({}): {} floor(s)", s.context.frameIndex(), ui::toString(tab),
                 via, s.directory.building().floors.size());
}

void ProductUi::openDirectoryCard(const ui::DirectoryGame& game, std::string_view via) {
    Impl& s = *m_impl;
    if (s.directory.isOpen() || s.catalogOpen) {
        return;
    }
    openDirectory(ui::DirectoryTab::Games, via);
    if (!s.directory.isOpen()) {
        return;
    }
    s.directory.openGame(game);
    s.addDirectoryEvent("open-card", game.gameId, std::string(via));
    spdlog::info("Frame {}: the directory opened at the card of {} ({})", s.context.frameIndex(), game.gameId, via);
}

void ProductUi::directoryBack() {
    Impl& s = *m_impl;
    if (s.directory.isOpen()) {
        s.directoryCommand(s.directory.apply(ui::UiCommand::Back));
    }
}

void ProductUi::closeDirectory() {
    Impl& s = *m_impl;
    if (!s.directory.isOpen() || panelOpen()) {
        return;
    }
    s.directory.close();
    s.addDirectoryEvent("closed");
    spdlog::info("Frame {}: the directory closed", s.context.frameIndex());
}

void ProductUi::setDirectoryBuilding(ui::DirectoryBuilding building) {
    Impl& s = *m_impl;
    s.directoryRecord.bound = true;
    if (s.directory.building() != building) {
        // I4b step 8: the chains' rooms as listed, their ranges as shown (the run report).
        s.directoryRecord.chainRooms.clear();
        for (const ui::DirectoryFloor& floor : building.floors) {
            for (const ui::DirectoryRoom& room : floor.rooms) {
                if (room.chainRooms < 2) {
                    continue;
                }
                DirectoryChainRoomRecord record{room.id, {}, room.scattered};
                for (const ui::DirectoryRange& range : room.ranges) {
                    record.ranges.push_back(DirectoryRangeRecord{range.system.name, range.first, range.last});
                }
                s.directoryRecord.chainRooms.push_back(std::move(record));
            }
        }
        s.directory.setBuilding(std::move(building));
    }
}

bool ProductUi::directoryCommandsPending() const noexcept {
    return !m_impl->directoryCommands.empty();
}

std::vector<ui::DirectoryCommand> ProductUi::takeDirectoryCommands() {
    return std::exchange(m_impl->directoryCommands, {});
}

void ProductUi::refuseDirectory(const ui::DirectoryCommand& command, std::string_view notice) {
    Impl& s = *m_impl;
    s.directory.reopen();
    s.directory.setNotice(std::string(notice));
    s.nav.reset();
    s.addDirectoryEvent("refused", std::string(notice), std::string(ui::toString(command.kind)));
    spdlog::info("Frame {}: the directory's {} '{}' refused for now ({})", s.context.frameIndex(),
                 ui::toString(command.kind), command.id, notice);
}

void ProductUi::setElevatorPrompt(std::optional<std::string> key, bool totem) {
    m_impl->wantedPrompt = std::move(key);
    m_impl->wantedTotem = totem;
}

bool ProductUi::menuOpen() const noexcept {
    return m_impl->menu.isOpen();
}

void ProductUi::openMenu(std::string_view via) {
    Impl& s = *m_impl;
    if (s.menu.isOpen()) {
        return;
    }
    if (!s.menuScreen) {
        auto screen = ui::SystemMenuScreen::create(*s.view);
        if (!screen) {
            spdlog::error("The menu unavailable: {}", screen.error().toString());
            return;
        }
        s.menuScreen = std::move(*screen);
    } else if (Rml::Element* root = s.view->context().GetRootElement()) {
        root->Focus(); // as the directory's: the focus the screen applies starts from the context's root
    }
    // A stick or direction still held from walking must not move in the panel that just opened.
    s.nav.reset();
    s.menu.open();
    ++s.menuRecord.opened;
    s.addMenuEvent("open", std::string(via));
    spdlog::info("Frame {}: the menu opened ({})", s.context.frameIndex(), via);
}

void ProductUi::menuBack() {
    Impl& s = *m_impl;
    if (s.settingsModel.isOpen()) {
        s.settingsCommand(s.settingsModel.apply(ui::UiCommand::Back));
    } else if (s.menu.isOpen()) {
        s.menuCommand(s.menu.apply(ui::UiCommand::Back));
    }
}

void ProductUi::setMenuHooks(MenuHooks hooks) {
    m_impl->menuHooks = std::move(hooks);
}

void ProductUi::refreshSettings() {
    Impl& s = *m_impl;
    if (s.settingsModel.isOpen() && s.menuHooks.settingRows) {
        s.settingsModel.setRows(s.menuHooks.settingRows());
    }
}

void ProductUi::setTextScale(float scale) {
    m_impl->view->setTextScale(scale);
}

bool ProductUi::handleEvent(const SDL_Event& event) {
    if (!hasInput()) {
        return false;
    }
    // I7 step 1: the menu and the settings screen likewise (only the mouse; their commands come from the UI actions).
    if (m_impl->settingsShown() && !m_impl->panel->visible()) {
        return m_impl->settingsScreen->handlePointer(event);
    }
    if (m_impl->menuShown() && !m_impl->panel->visible()) {
        return m_impl->menuScreen->handlePointer(event);
    }
    // I4b step 6: the regeneration's preview likewise (only the mouse; its choices come from the UI actions).
    if (m_impl->previewShown() && !m_impl->panel->visible() && !m_impl->screensOpen()) {
        return m_impl->previewScreen->handlePointer(event);
    }
    // The directory (I4 step 7b): only the mouse reaches it (its commands come from the UI actions), unless the
    // game-session panel or the media screens show over it.
    if (m_impl->directoryShown() && !m_impl->panel->visible() && !m_impl->screensOpen()) {
        return m_impl->directoryScreen->handlePointer(event);
    }
    // Over the media viewer and Informazioni only the mouse reaches RmlUi (their buttons, the wheel): keys and text
    // must not reach the quick catalog's search field under them (I3 step 14b).
    if (m_impl->screensOpen() && event.type != SDL_EVENT_MOUSE_MOTION && event.type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.type != SDL_EVENT_MOUSE_BUTTON_UP && event.type != SDL_EVENT_MOUSE_WHEEL &&
        event.type != SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        return false;
    }
    return m_impl->view->handleEvent(event);
}

void ProductUi::focusChanged(bool focused) {
    // A hidden window keeps counting as focused (create()).
    if (!m_impl->context.options().hidden) {
        m_impl->flow.focusChanged(focused, LaunchClock::now());
    }
}

void ProductUi::userPressed() {
    static_cast<void>(m_impl->flow.userPressed());
}

void ProductUi::typeText(std::string_view text) {
    if (hasInput() && !text.empty()) {
        m_impl->view->context().ProcessTextInput(Rml::String(text));
    }
}

void ProductUi::setLaunchHooks(LaunchHooks hooks) {
    m_impl->hooks = std::move(hooks);
}

bool ProductUi::launchWorldGame(const catalog::GameId& game, std::string_view title, bool held) {
    Impl& s = *m_impl;
    ui::CatalogEntry entry;
    entry.id = game.toString();
    entry.title = std::string(title);
    const std::optional<std::uint64_t> token = s.launch(entry);
    if (!token) {
        return false;
    }
    s.worldToken = token;
    s.worldEnded.reset();
    s.worldHeld = held;
    s.heldDecision.reset();
    spdlog::info("Frame {}: Avvia in the room on '{}' ({}): launch {} asked{}", s.context.frameIndex(), entry.title,
                 entry.id, *token, held ? ", its hand-over after the insertion" : "");
    if (held && s.hooks.preparing) {
        // The decoders' stop and the sound's fade run while the item goes into the console (ADR-0016 §11).
        s.worldPrepared = true;
        s.hooks.preparing(entry.id);
    }
    return true;
}

void ProductUi::releaseWorldLaunch() {
    Impl& s = *m_impl;
    if (!s.worldHeld) {
        return;
    }
    s.worldHeld = false;
    if (std::optional<LaunchDecision> decision = std::exchange(s.heldDecision, std::nullopt)) {
        s.applyDecision(std::move(*decision));
    }
}

bool ProductUi::worldLaunchInFlight() const noexcept {
    return m_impl->worldToken.has_value();
}

std::optional<LaunchMessage> ProductUi::takeWorldLaunchEnded() {
    return std::exchange(m_impl->worldEnded, std::nullopt);
}

void ProductUi::setRoomPlacement(std::function<std::string(std::string_view gameId)> placement) {
    m_impl->roomPlacement = std::move(placement);
    m_impl->applyRoomPlacement();
}

std::optional<std::string> ProductUi::takeShowInRoom() {
    return std::exchange(m_impl->showInRoomAsked, std::nullopt);
}

void ProductUi::setWorldNotice(WorldNoticeContent content) {
    m_impl->wantedNotice = std::move(content);
}

void ProductUi::setEditBar(std::optional<EditBarContent> content) {
    m_impl->wantedEditBar = std::move(content);
}

void ProductUi::setWorldLabel(std::optional<WorldLabelContent> content) {
    m_impl->wantedLabel = std::move(content);
}

void ProductUi::setWorldCard(std::optional<ui::GameCardContent> content, ui::CardButton focused) {
    m_impl->wantedCard = std::move(content);
    m_impl->wantedCardFocus = focused;
}

bool ProductUi::handleWorldCardEvent(const SDL_Event& event) {
    return m_impl->card && m_impl->card->handlePointer(event);
}

std::optional<ui::CardButton> ProductUi::takeWorldCardClick() {
    return m_impl->card ? m_impl->card->takeClicked() : std::nullopt;
}

bool ProductUi::worldCardSqueezed() const {
    return m_impl->card && m_impl->card->squeezed();
}

std::optional<glm::vec4> ProductUi::worldCardCoverBox() const {
    return m_impl->card ? m_impl->card->coverBox() : std::nullopt;
}

void ProductUi::setUiTexture(std::string_view key, unsigned texture, glm::ivec2 size) {
    m_impl->system->setTexture(key, texture, size);
}

void ProductUi::openGameMedia(const catalog::GameId& game, std::string_view title, std::string_view from) {
    m_impl->openMedia(game.toString(), title, std::nullopt, from);
}

void ProductUi::openGameInfo(const catalog::GameId& game, std::string_view title, std::string_view from) {
    m_impl->openInfo(game.toString(), title, std::nullopt, from);
}

void ProductUi::setVersionLookup(std::function<std::vector<catalog::GameId>(const catalog::GameId&)> lookup) {
    m_impl->versionLookup = std::move(lookup);
}

bool ProductUi::gameScreenOpen() const noexcept {
    return m_impl->screensOpen();
}

bool ProductUi::mediaViewerOpen() const noexcept {
    return m_impl->screens && m_impl->screens->viewerOpen();
}

ViewerRequest ProductUi::viewerRequest() const {
    return m_impl->screens ? m_impl->screens->viewerRequest() : ViewerRequest{};
}

void ProductUi::setViewerVideo(const ViewerVideo& video) {
    if (m_impl->screens) {
        m_impl->screens->setViewerVideo(video, m_impl->context.frameIndex());
    }
}

std::uint64_t ProductUi::catalogRevision() const noexcept {
    return m_impl->catalogRevision;
}

AppCatalog& ProductUi::catalog() noexcept {
    return *m_impl->catalog;
}

std::optional<std::string> ProductUi::selectedGameId() const {
    const Impl& s = *m_impl;
    if (!s.catalogOpen) {
        return std::nullopt;
    }
    if (const ui::CatalogEntry* selected = s.model->selected()) {
        return selected->id;
    }
    return std::nullopt;
}

void ProductUi::update(const input::ActionState* uiActions, double deltaSeconds, double timeSeconds) {
    const core::CpuSectionScope timing(core::cpu_section::kProductUi);
    Impl& s = *m_impl;
    const auto frameStart = std::chrono::steady_clock::now();
    if (s.lastFrame && s.catalogOpen && !s.model->window().empty() && s.frameMs.size() < Impl::kMaxFrameSamples) {
        s.frameMs.push_back(std::chrono::duration<double, std::milli>(frameStart - *s.lastFrame).count());
    }
    s.lastFrame = frameStart;
    if (const auto since = std::exchange(s.catalogWaitSince, std::nullopt);
        since && frameStart - *since >= Impl::kLockstepWaitLimit && s.catalog->busy()) {
        spdlog::warn("Frame {}: the catalog worker did not answer within {} s; the frame goes on without its answer",
                     s.context.frameIndex(), Impl::kLockstepWaitLimit.count());
    }
    // The first frame after a launch ended (or was refused): what the catalog and the walker show now.
    if (std::exchange(s.afterPending, false)) {
        s.flow.noteAfter(s.currentView());
    }
    s.syncCatalog();
    // A session whose panel shows renders frames: it is followed every frame (it may end, or ask something else).
    s.flow.poll(LaunchClock::now(), s.context.frameIndex());
    s.afterReturn();
    s.followHandover();

    s.commands.clear();
    if (uiActions != nullptr) {
        // I4b step 6's review: the preview's hints show the glyphs of the device used last.
        s.preview.setDevice(uiActions->activeDevice() == input::DeviceKind::Gamepad);
        s.settingsModel.setDevice(uiActions->activeDevice() == input::DeviceKind::Gamepad);
    }
    const auto followPreviewButtons = [&] {
        if (std::vector<ui::RegenerateButton> buttons = s.preview.buttons(); buttons != s.previewButtonsSeen) {
            s.previewButtonsSeen = std::move(buttons);
            s.previewButtonsSince = timeSeconds;
        }
    };
    followPreviewButtons();
    if (uiActions != nullptr && hasInput()) {
        s.nav.update(*uiActions, deltaSeconds, s.commands);
        for (const ui::UiCommand command : s.commands.commands()) {
            spdlog::debug("Frame {}: UI command {}{}", s.context.frameIndex(), ui::toString(command),
                          s.panel->visible() ? " (game-session panel)" : "");
            if (command == ui::UiCommand::Back) {
                // The application's levels: the developer overlay first, then panelBack() or catalogBack().
                if (s.back) {
                    s.back();
                }
            } else if (s.panel->visible()) {
                s.panel->apply(command);
            } else if (s.preview.isOpen()) {
                s.previewCommand(s.preview.apply(command)); // I4b step 6
            } else if (s.screensOpen()) {
                s.switchScreen(s.screens->apply(command, s.context.frameIndex()));
            } else if (s.settingsModel.isOpen()) {
                s.settingsCommand(s.settingsModel.apply(command)); // I7 step 1
            } else if (s.menu.isOpen()) {
                s.menuCommand(s.menu.apply(command)); // I7 step 1
            } else if (s.catalogOpen && s.screen) {
                s.screen->apply(command);
            } else if (s.directoryShown()) {
                s.directoryCommand(s.directory.apply(command));
            }
        }
    }
    // The directory's clicks (I4 step 7b): a tab, a row, a card's button.
    if (s.directoryScreen) {
        if (const std::optional<ui::DirectoryClick> click = s.directoryScreen->takeClicked();
            click && s.directoryShown() && !s.panel->visible()) {
            s.directoryCommand(s.directory.click(*click));
        }
    }
    // I7 step 1: the menu's and the settings screen's clicks (VR's ray among them).
    if (s.settingsScreen) {
        if (const std::optional<ui::SettingsClick> click = s.settingsScreen->takeClicked();
            click && s.settingsShown() && !s.panel->visible()) {
            s.settingsCommand(s.settingsModel.click(*click));
        }
    }
    if (s.menuScreen) {
        if (const std::optional<ui::SystemMenuButton> click = s.menuScreen->takeClicked();
            click && s.menuShown() && !s.panel->visible()) {
            s.menuCommand(s.menu.click(*click));
        }
    }
    // I4b step 6: the preview's clicks (but the second of a double-click, whose button changed under it).
    followPreviewButtons();
    if (s.previewScreen) {
        if (const std::optional<ui::RegenerateButton> click = s.previewScreen->takeClicked();
            click && s.previewShown() && !s.panel->visible()) {
            if (timeSeconds - s.previewButtonsSince < Impl::kPreviewClickGuardSeconds) {
                spdlog::info("Frame {}: a click on the preview's {} ignored (its buttons changed {:.2f} s ago)",
                             s.context.frameIndex(), ui::toString(*click), timeSeconds - s.previewButtonsSince);
            } else {
                s.previewCommand(s.preview.click(*click));
            }
        }
    }
    followPreviewButtons();
    // This input frame's commands are applied: a content that arrives after them (the world's update) ignores the
    // next input frame's Activate.
    s.preview.endInputFrame();
    if (std::exchange(s.refreshRequested, false)) {
        s.requestRefresh(RefreshTrigger::User);
    }
    s.applyDecision();
    s.applyChoice();
    s.afterReturn(); // a return the panel confirmed
    if (std::exchange(s.closeRequested, false)) {
        closeCatalog();
    }
    if (s.screens) {
        s.switchScreen(s.screens->update(s.context.frameIndex()));
    }
    s.syncPanel();
    s.syncDirectory();
    s.syncNotice();
    s.syncLabel();
    s.syncCard();
    s.syncPrompt();
    s.syncEditBar();
    s.syncPreview();
    s.syncMenu();

    if (s.visible()) {
        s.system->setTime(timeSeconds);
        s.view->resize(s.context.framebufferSize(), s.context.window().displayScale());
        s.view->update();
        if (s.screens) {
            s.screens->afterUpdate();
        }
        if (s.directoryShown()) {
            s.directoryScreen->afterUpdate();
        }
        if (s.previewShown()) {
            s.previewScreen->afterUpdate();
        }
        if (s.menuShown()) {
            s.menuScreen->afterUpdate();
            s.menuRecord.textCut = std::max(s.menuRecord.textCut, s.menuScreen->textCut());
        }
        if (s.settingsShown()) {
            s.settingsScreen->afterUpdate();
            s.menuRecord.settingsTextCut = std::max(s.menuRecord.settingsTextCut, s.settingsScreen->textCut());
        }
        if (s.screen) {
            s.screen->setNotice(s.catalog->notice());
            s.screen->setRefresh(shownRefresh(s.catalog->refreshState()));
            s.screen->afterUpdate();
            if constexpr (kWatchDocuments) {
                s.screen->reloadIfChanged();
            }
            if (s.catalogOpen && !s.model->window().empty() && s.catalog->notice().empty()) {
                s.catalog->pageShown();
            }
        }
    }
}

void ProductUi::panelBack() {
    // I4b step 6: the regeneration's preview leaves one level (the second confirmation, then the panel), unless the
    // game-session panel shows over it.
    if (m_impl->preview.isOpen() && !m_impl->panel->visible() && m_impl->flow.panel() == SessionPanel::None) {
        m_impl->previewCommand(m_impl->preview.apply(ui::UiCommand::Back));
        return;
    }
    // The media viewer and Informazioni close first (I3 step 14b), unless the game-session panel shows over them.
    if (m_impl->screensOpen() && !m_impl->panel->visible()) {
        m_impl->screens->close(m_impl->context.frameIndex());
        return;
    }
    if (m_impl->flow.panelBack(LaunchClock::now())) {
        m_impl->handForegroundToGame();
    }
}

void ProductUi::catalogBack() {
    if (m_impl->catalogOpen && m_impl->screen) {
        m_impl->screen->apply(ui::UiCommand::Back);
    }
}

bool ProductUi::wantsIdle() const noexcept {
    return m_impl->flow.hostIdle() || m_impl->waitsForCatalog();
}

bool ProductUi::launchActive() const noexcept {
    return m_impl->flow.pending() || m_impl->flow.hostIdle();
}

void ProductUi::onIdle() {
    Impl& s = *m_impl;
    s.lastFrame.reset(); // the wait is no frame time
    if (s.lockstep && !s.catalogWaitSince && !s.flow.hostIdle() &&
        (s.catalog->busy() || (s.screens && s.screens->loading()))) {
        s.catalogWaitSince = std::chrono::steady_clock::now();
    }
    // A decision the worker brings is applied by the next frame's update, which renders its "Gioco in corso" first.
    s.syncCatalog();
    const LaunchClock::time_point now = LaunchClock::now();
    const std::uint64_t frame = s.context.frameIndex();
    if (s.flow.pending()) {
        s.worldPrepared = false; // started: `ended` comes with the return (afterReturn())
        if (s.hooks.starting) {
            s.hooks.starting(); // before the hand-over and the process (the decoder stopped and joined)
        }
        s.beginHandover();
        launcher::StartCallbacks callbacks;
        callbacks.wake = s.context.wakeCallback();
        callbacks.beforeResume = [state = &s](std::uint32_t /*processId*/) {
            if (state->handover) {
                state->handover->handOver(); // Keep: the window stays behind the emulator, rendering nothing
            }
        };
        s.flow.start(now, std::move(callbacks), frame);
    } else {
        s.flow.idleWake();
    }
    s.flow.poll(LaunchClock::now(), frame);
    s.afterReturn();
}

void ProductUi::frameRendered() noexcept {
    m_impl->flow.frameRendered();
}

void ProductUi::xrPointer(const std::optional<glm::vec2>& pixel, bool pressed, bool released, float wheel) {
    Rml::Context& context = m_impl->view->context();
    if (!pixel) {
        if (m_impl->xrPointing) {
            context.ProcessMouseLeave();
            m_impl->xrPointing = false;
        }
        return;
    }
    m_impl->xrPointing = true;
    context.ProcessMouseMove(static_cast<int>(pixel->x), static_cast<int>(pixel->y), 0);
    if (pressed) {
        context.ProcessMouseButtonDown(0, 0);
    }
    if (released) {
        context.ProcessMouseButtonUp(0, 0);
    }
    if (wheel != 0.0f) {
        context.ProcessMouseWheel(wheel, 0);
    }
}

std::uintptr_t ProductUi::launchWindow() const noexcept {
    const launcher::LaunchSession* session = m_impl->flow.session();
    return session != nullptr && !session->finished() ? session->window() : 0;
}

bool ProductUi::visible() const noexcept {
    return m_impl->visible();
}

void ProductUi::renderToWindow(gl::GLDevice& device) {
    if (m_impl->visible()) {
        const core::CpuSectionScope timing(core::cpu_section::kProductUi);
        m_impl->view->render(device);
    }
}

void ProductUi::renderTo(gl::GLDevice& device, const gl::Framebuffer& target) {
    if (m_impl->visible()) {
        const core::CpuSectionScope timing(core::cpu_section::kProductUi);
        m_impl->view->renderTo(device, target);
    }
}

void ProductUi::finish() {
    Impl& s = *m_impl;
    s.flow.quit(LaunchClock::now(), s.context.frameIndex());
    s.handover.reset();
    if (std::exchange(s.afterPending, false)) {
        s.flow.noteAfter(s.currentView());
    }
    // The worker ends (within its bound) once the sessions the quit just ended are queued to it; a refresh still running
    // is cancelled, its transaction rolled back.
    s.catalog->shutdown();
    if (!s.refreshes.empty() && s.refreshes.back().outcome == ui::quick_catalog::kRefreshRunning) {
        RefreshRecord& running = s.refreshes.back();
        const RefreshState state = s.catalog->refreshState();
        if (!state.running && state.last) {
            recordRefresh(running, *state.last, state.lastLists);
        }
        running.stopMs = s.catalog->stopMilliseconds();
        spdlog::info("Catalog refresh {} at the end of the run: the worker stopped in {:.0f} ms", running.outcome,
                     running.stopMs.value_or(0.0));
    }
    CatalogRecord record;
    s.catalog->record(record);
    record.refreshes = std::move(s.refreshes);
    record.opened = s.opened;
    record.open = s.catalogOpen;
    record.selectedPosition = s.model->selectedPosition();
    if (const ui::CatalogEntry* selected = s.model->selected()) {
        record.selectedId = selected->id;
    }
    record.search = s.model->search();
    record.filter = s.model->activeFilter();
    record.listed = s.model->count();
    if (s.screen && s.catalogOpen) {
        record.cardSqueezed = s.screen->squeezedCardBlocks();
        record.shownCount = s.screen->shownCount();
    }
    if (!s.lockstep) {
        record.frames = summarizeTimes(s.frameMs);
    }
    RunReport& report = s.context.report();
    report.catalog = std::move(record);
    report.launches = s.flow.records();
    if (s.screens) {
        report.screens = s.screens->record();
    }
    s.menuRecord.open = s.menu.isOpen();
    s.menuRecord.view = std::string(ui::toString(s.menu.view()));
    s.menuRecord.settingsOpen = s.settingsModel.isOpen();
    s.menuRecord.settingsGroup = s.settingsModel.group();
    for (const ui::SettingsRow& row : s.settingsModel.rows()) {
        s.menuRecord.settingsRows.push_back(row.key);
    }
    report.menu = std::move(s.menuRecord);
    if (s.directoryRecord.bound) {
        DirectoryRecord directory = std::move(s.directoryRecord);
        directory.open = s.directory.isOpen();
        directory.tab = std::string(ui::toString(s.directory.tab()));
        directory.view = std::string(ui::toString(s.directory.view()));
        directory.notice = s.directory.notice();
        directory.gamesState = std::string(ui::toString(s.directory.gamesState()));
        report.directory = std::move(directory);
    }
    for (std::size_t index = 0; index < report.launches.size(); ++index) {
        s.catalog->recordLaunch(index, report.launches[index]);
    }
}

} // namespace rv::app
