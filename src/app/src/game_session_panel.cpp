#include "game_session_panel.hpp"

#include <string>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <spdlog/spdlog.h>

#include <rv/app/game_session_ui.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::app {
namespace {

namespace gs = game_session_ui;

/// The button a phase focuses when it shows (none for Launching, which offers no choice).
[[nodiscard]] std::string_view defaultChoice(SessionPanel panel) noexcept {
    switch (panel) {
    case SessionPanel::Playing:
    case SessionPanel::StillRunning:
    case SessionPanel::Confirm:
        return gs::kReturnId;
    case SessionPanel::Outcome:
        return gs::kOkId;
    default:
        return {};
    }
}

} // namespace

struct GameSessionPanel::Impl {
    Impl(ui::UiView& view, GameSessionPanelEvents events) : view(view), events(std::move(events)) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(gs::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(gs::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(gs::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("phase", &phase);
        constructor.Bind("title", &title);
        constructor.Bind("code", &code);
        constructor.Bind("reason", &reason);
        constructor.Bind("detail", &detail);
        constructor.Bind("timeout", &timeout);
        constructor.Bind("exclusive", &exclusive);
        // The buttons act on the panel shown when they were pressed: the application decides what follows.
        constructor.BindEventCallback("return_home", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (shown != SessionPanel::None && events.returnHome) {
                events.returnHome();
            }
        });
        constructor.BindEventCallback("resume", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (shown != SessionPanel::None && events.resume) {
                events.resume();
            }
        });
        constructor.BindEventCallback("dismiss", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (shown != SessionPanel::None && events.dismiss) {
                events.dismiss();
            }
        });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(gs::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the game-session panel's document could not be loaded (see the log)",
                             std::string(gs::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    ui::UiView& view;
    GameSessionPanelEvents events;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    SessionPanel shown = SessionPanel::None;
    Rml::String phase;
    Rml::String title;
    Rml::String code;
    Rml::String reason;
    Rml::String detail;
    int timeout = 0;
    bool exclusive = false;
};

GameSessionPanel::GameSessionPanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

GameSessionPanel::~GameSessionPanel() = default;

Expected<std::unique_ptr<GameSessionPanel>> GameSessionPanel::create(ui::UiView& view, GameSessionPanelEvents events) {
    auto impl = std::make_unique<Impl>(view, std::move(events));
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<GameSessionPanel>(new GameSessionPanel(std::move(impl)));
}

void GameSessionPanel::show(SessionPanel panel, std::string_view title, const LaunchMessage& message,
                            int timeoutSeconds) {
    Impl& impl = *m_impl;
    if (panel == SessionPanel::None) {
        hide();
        return;
    }
    if (impl.document == nullptr) {
        return;
    }
    impl.shown = panel;
    impl.phase = Rml::String(toString(panel));
    impl.title = Rml::String(title);
    impl.code = message.code;
    impl.reason = message.reason;
    impl.detail = message.detail;
    impl.timeout = timeoutSeconds;
    impl.handle.DirtyAllVariables();
    // Modal: the catalog behind it can take neither the focus nor the mouse. The document itself takes the focus first
    // (a phase without choices keeps it), in front of the catalog.
    impl.document->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
    impl.document->PullToFront();
    // The phase's elements exist once the data views have run: lay the document out now, then focus its first choice.
    impl.view.context().Update();
    if (const std::string_view choice = defaultChoice(panel); !choice.empty()) {
        if (Rml::Element* button = impl.document->GetElementById(Rml::String(choice))) {
            button->Focus(true);
        }
    }
    spdlog::info("Game-session panel: {}{}", toString(panel),
                 panel == SessionPanel::Outcome ? " (" + message.code +
                                                      (message.reason.empty() ? std::string() : ", " + message.reason) +
                                                      ")"
                                                : std::string());
}

void GameSessionPanel::hide() {
    Impl& impl = *m_impl;
    if (impl.shown == SessionPanel::None || impl.document == nullptr) {
        impl.shown = SessionPanel::None;
        return;
    }
    impl.shown = SessionPanel::None;
    // Modal no more, so that RmlUi hands the focus back to the document focused before (the catalog's element).
    impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    impl.document->Hide();
}

SessionPanel GameSessionPanel::shown() const noexcept {
    return m_impl->shown;
}

void GameSessionPanel::apply(ui::UiCommand command) {
    Impl& impl = *m_impl;
    if (impl.shown == SessionPanel::None) {
        return;
    }
    if (const std::optional<ui::NavDirection> direction = ui::directionOf(command)) {
        static_cast<void>(impl.view.navigate(*direction));
        return;
    }
    if (command == ui::UiCommand::Activate) {
        static_cast<void>(impl.view.activate());
    }
}

void GameSessionPanel::setExclusiveFullscreen(bool exclusive) {
    if (m_impl->exclusive == exclusive) {
        return;
    }
    m_impl->exclusive = exclusive;
    if (m_impl->handle) {
        m_impl->handle.DirtyVariable("exclusive");
    }
}

void GameSessionPanel::setHighContrast(bool enabled) {
    if (m_impl->document != nullptr) {
        m_impl->document->SetClass("high-contrast", enabled);
    }
}

} // namespace rv::app
