#include "product_ui.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL_events.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/launch_plan.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/window_handover.hpp>
#include <rv/platform_win/system_probe.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/quick_catalog_screen.hpp>
#include <rv/ui/ui_command.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

#include "game_session_panel.hpp"

namespace rv::app {
namespace {

namespace lo = launch_outcome;
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
    Impl(AppContext& context, MockCatalog catalog, std::function<void()> back)
        : context(context), back(std::move(back)), catalog(std::move(catalog)) {}

    // Declared in the order they are created; destroyed in reverse: the screens before the model and the view, the
    // model before its source, the view before RmlUi, the launch (its session) before the runner it started from.
    AppContext& context;
    std::function<void()> back;
    MockCatalog catalog;
    launcher::EmulatorProfiles profiles;
    platform_win::Win32SystemProbe probe;
    platform_win::Win32ProcessRunner runner;
    LaunchFlow flow{runner};
    std::optional<platform::WindowHandover> handover;
    /// The window is coming back after a launch (WindowHandover::update every frame until it is).
    bool restoring = false;

    std::unique_ptr<ui::UiSystem> system;
    std::unique_ptr<ui::UiView> view;
    std::unique_ptr<ui::InMemoryCatalogSource> source;
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

    [[nodiscard]] bool visible() const noexcept { return catalogOpen || panel->visible(); }

    /// Avvia on `entry`: its launch planned with its profile, then requested, or refused with the reason.
    void launch(const ui::CatalogEntry& entry) {
        if (flow.pending() || flow.sessionActive()) {
            return;
        }
        const std::uint64_t frame = context.frameIndex();
        LaunchInfo info{entry.id, entry.title, {}};
        const MockGame* game = catalog.find(entry.id);
        if (game == nullptr || !game->launch) {
            flow.refuse(LaunchMessage{std::string(lo::kNotConfigured), std::string(lo::kReasonNoLaunch), {}},
                        std::move(info), frame);
            return;
        }
        const MockLaunch& target = *game->launch;
        info.profile = target.profile;
        const launcher::EmulatorProfile* profile = profiles.find(target.profile);
        if (profile == nullptr) {
            flow.refuse(LaunchMessage{std::string(lo::kNotConfigured), std::string(lo::kReasonUnknownProfile),
                                      target.profile},
                        std::move(info), frame);
            return;
        }
        launcher::LaunchPlan plan = launcher::planLaunch(target.target, *profile, probe);
        if (!plan.ready()) {
            spdlog::warn("Launch of '{}' blocked ({}): {}", entry.title, launcher::toString(plan.blocker->kind),
                         plan.blocker->message);
            flow.refuse(messageForBlocker(*plan.blocker, target.target, profile->displayName), std::move(info), frame);
            return;
        }
        flow.request(std::move(*plan.request), std::move(info), frame);
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
    /// held directions are forgotten (the input of the wait was dropped).
    void afterReturn() {
        if (!flow.takeReturned()) {
            return;
        }
        nav.reset();
        if (handover) {
            handover->restore();
            restoring = true;
        }
    }

    void followHandover() {
        if (!handover || !restoring) {
            return;
        }
        if (handover->update()) {
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
            flow.backToGame();
            break;
        case PanelChoice::Dismiss:
            flow.dismissOutcome();
            break;
        }
    }
};

ProductUi::ProductUi(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

ProductUi::~ProductUi() = default;

Expected<std::unique_ptr<ProductUi>> ProductUi::create(AppContext& context, MockCatalog catalog,
                                                       const input::ActionMap& bindings, std::function<void()> back) {
    const core::Paths& paths = context.paths();
    auto impl = std::make_unique<Impl>(context, std::move(catalog), std::move(back));
    Impl& s = *impl;
    s.profiles = mockCatalogProfiles(launcher::loadEmulatorProfiles(paths), s.catalog);
    for (const launcher::ProfileIssue& issue : s.catalog.profiles.issues) {
        spdlog::warn("Mock catalog profiles: {}", issue.toString());
    }

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

    s.source = std::make_unique<ui::InMemoryCatalogSource>(s.catalog.entries());
    s.model = std::make_unique<ui::QuickCatalogModel>(*s.source);
    ui::QuickCatalogEvents events;
    Impl* state = impl.get();
    events.launch = [state](const ui::CatalogEntry& entry) { state->launch(entry); };
    events.close = [state] { state->closeRequested = true; };
    auto screen = ui::QuickCatalogScreen::create(*s.view, *s.model, std::move(events), kWatchDocuments);
    if (!screen) {
        return propagate(screen.error());
    }
    s.screen = std::move(*screen);
    s.screen->hide(); // the catalog starts closed: QuickCatalog (Tab, gamepad Y) opens it

    GameSessionPanelEvents panelEvents;
    panelEvents.returnHome = [state] { state->choice = PanelChoice::ReturnHome; };
    panelEvents.resume = [state] { state->choice = PanelChoice::Resume; };
    panelEvents.dismiss = [state] { state->choice = PanelChoice::Dismiss; };
    auto panel = GameSessionPanel::create(*s.view, std::move(panelEvents));
    if (!panel) {
        return propagate(panel.error());
    }
    s.panel = std::move(*panel);
    spdlog::info("Quick catalog: {} game(s) from the mock catalog {}, {} emulator profile(s) in effect; QuickCatalog "
                 "(Tab, gamepad Y) opens it",
                 s.source->count(), core::pathToUtf8Lossy(s.catalog.file), s.profiles.profiles.size());
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
    s.screen->show();
    // A stick or direction still held from walking must not start scrolling the catalog that just opened.
    s.nav.reset();
    s.catalogOpen = true;
    ++s.opened;
    spdlog::info("Quick catalog opened ({} game(s))", s.model->count());
}

void ProductUi::closeCatalog() {
    Impl& s = *m_impl;
    if (!s.catalogOpen || panelOpen()) {
        return;
    }
    s.screen->hide();
    s.catalogOpen = false;
    spdlog::info("Quick catalog closed");
}

bool ProductUi::handleEvent(const SDL_Event& event) {
    return hasInput() && m_impl->view->handleEvent(event);
}

void ProductUi::focusChanged(bool focused) {
    m_impl->flow.focusChanged(focused, LaunchClock::now());
}

void ProductUi::update(const input::ActionState* uiActions, double deltaSeconds, double timeSeconds) {
    Impl& s = *m_impl;
    // A session whose panel shows renders frames: it is followed every frame (it may end, or ask something else).
    s.flow.poll(LaunchClock::now(), s.context.frameIndex());
    s.afterReturn();
    s.followHandover();

    s.commands.clear();
    if (uiActions != nullptr && hasInput()) {
        s.nav.update(*uiActions, deltaSeconds, s.commands);
        for (const ui::UiCommand command : s.commands.commands()) {
            if (command == ui::UiCommand::Back) {
                // The application's levels: the developer overlay first, then panelBack() or catalogBack().
                if (s.back) {
                    s.back();
                }
            } else if (s.panel->visible()) {
                s.panel->apply(command);
            } else if (s.catalogOpen) {
                s.screen->apply(command);
            }
        }
    }
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
        s.screen->afterUpdate();
        if constexpr (kWatchDocuments) {
            s.screen->reloadIfChanged();
        }
    }
}

void ProductUi::panelBack() {
    m_impl->flow.panelBack();
}

void ProductUi::catalogBack() {
    if (m_impl->catalogOpen) {
        m_impl->screen->apply(ui::UiCommand::Back);
    }
}

bool ProductUi::wantsIdle() const noexcept {
    return m_impl->flow.hostIdle();
}

void ProductUi::onIdle() {
    Impl& s = *m_impl;
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
    CatalogRecord record;
    record.source = "mock";
    record.file = s.catalog.file;
    record.games = s.source->entries().size();
    record.opened = s.opened;
    record.open = s.catalogOpen;
    record.selectedPosition = s.model->selectedPosition();
    if (const ui::CatalogEntry* selected = s.model->selected()) {
        record.selectedId = selected->id;
    }
    RunReport& report = s.context.report();
    report.catalog = std::move(record);
    report.launches = s.flow.records();
}

} // namespace rv::app
