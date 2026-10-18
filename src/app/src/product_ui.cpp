#include "product_ui.hpp"

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
#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/window_handover.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/quick_catalog_screen.hpp>
#include <rv/ui/ui_command.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

#include "game_session_panel.hpp"

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
    std::unique_ptr<ui::QuickCatalogModel> model;
    std::unique_ptr<ui::QuickCatalogScreen> screen;
    std::unique_ptr<GameSessionPanel> panel;

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
    /// The wall-clock times of the frames that showed games of the catalog (CatalogRecord::frames; at most
    /// kMaxFrameSamples), and when the last frame's update began (none after an idle wait, which is no frame time).
    std::vector<double> frameMs;
    std::optional<std::chrono::steady_clock::time_point> lastFrame;
    static constexpr std::size_t kMaxFrameSamples = 100000;

    [[nodiscard]] bool visible() const noexcept { return catalogOpen || panel->visible(); }

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
    void launch(const ui::CatalogEntry& entry) {
        if (flow.pending() || flow.sessionActive() || catalog->deciding()) {
            return;
        }
        const std::uint64_t token = flow.records().size();
        if (catalog->requestLaunch(token, entry)) {
            before = currentView();
        }
    }

    /// A decision the catalog made: the launch requested, or refused with its message (and its play session).
    void applyDecision() {
        std::optional<LaunchDecision> decision = catalog->takeDecision();
        if (!decision) {
            return;
        }
        const std::uint64_t frame = context.frameIndex();
        if (decision->request) {
            sessionToken = decision->token;
            flow.request(std::move(*decision->request), std::move(decision->info), frame);
        } else if (flow.refuse(std::move(decision->refusal), std::move(decision->info), frame)) {
            catalog->sessionRefused(decision->token);
            afterPending = true;
        }
        if (before) {
            flow.noteBefore(std::move(*before));
            before.reset();
        }
    }

    /// What the catalog worker answered: the catalog opened (the platforms and the list read again), rows that show
    /// other data now (read again), the list of a query (collected by the model, which the screen shows after its
    /// update). Polled here rather than only by the screen, which does not exist before the catalog first opens.
    void syncCatalog() {
        const CatalogChanges changes = catalog->poll();
        if (changes.opened) {
            model->refresh();
        }
        if (changes.rowsChanged) {
            model->reread();
        }
        model->poll();
    }

    /// The lockstep run waits for the catalog worker (within kLockstepWaitLimit).
    [[nodiscard]] bool waitsForCatalog() const noexcept {
        if (!lockstep || !catalog->busy()) {
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
    spdlog::info("Quick catalog ready{}; QuickCatalog (Tab, gamepad Y) opens it",
                 s.lockstep ? " (lockstep: frames wait for the catalog worker's answers)" : "");
    return std::unique_ptr<ProductUi>(new ProductUi(std::move(impl)));
}

bool ProductUi::catalogOpen() const noexcept {
    return m_impl->catalogOpen;
}

bool ProductUi::panelOpen() const noexcept {
    return m_impl->panel->visible() || m_impl->flow.panel() != SessionPanel::None;
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
        events.close = [state] { state->closeRequested = true; };
        auto screen = ui::QuickCatalogScreen::create(*s.view, *s.model, std::move(events), kWatchDocuments);
        if (!screen) {
            spdlog::error("Quick catalog unavailable: {}", screen.error().toString());
            return;
        }
        s.screen = std::move(*screen);
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

bool ProductUi::handleEvent(const SDL_Event& event) {
    return hasInput() && m_impl->view->handleEvent(event);
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

void ProductUi::update(const input::ActionState* uiActions, double deltaSeconds, double timeSeconds) {
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
            } else if (s.catalogOpen && s.screen) {
                s.screen->apply(command);
            }
        }
    }
    s.applyDecision();
    s.applyChoice();
    s.afterReturn(); // a return the panel confirmed
    if (std::exchange(s.closeRequested, false)) {
        closeCatalog();
    }
    s.syncPanel();

    if (s.visible()) {
        s.system->setTime(timeSeconds);
        s.view->resize(s.context.framebufferSize(), s.context.window().displayScale());
        s.view->update();
        if (s.screen) {
            s.screen->setNotice(s.catalog->notice());
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

void ProductUi::onIdle() {
    Impl& s = *m_impl;
    s.lastFrame.reset(); // the wait is no frame time
    if (s.lockstep && !s.catalogWaitSince && !s.flow.hostIdle() && s.catalog->busy()) {
        s.catalogWaitSince = std::chrono::steady_clock::now();
    }
    // A decision the worker brings is applied by the next frame's update, which renders its "Gioco in corso" first.
    s.syncCatalog();
    const LaunchClock::time_point now = LaunchClock::now();
    const std::uint64_t frame = s.context.frameIndex();
    if (s.flow.pending()) {
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

void ProductUi::renderToWindow(gl::GLDevice& device) {
    if (m_impl->visible()) {
        m_impl->view->render(device);
    }
}

void ProductUi::renderTo(gl::GLDevice& device, const gl::Framebuffer& target) {
    if (m_impl->visible()) {
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
    // The worker ends (within its bound) once the sessions the quit just ended are queued to it.
    s.catalog->shutdown();
    CatalogRecord record;
    s.catalog->record(record);
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
    for (std::size_t index = 0; index < report.launches.size(); ++index) {
        s.catalog->recordLaunch(index, report.launches[index]);
    }
}

} // namespace rv::app
