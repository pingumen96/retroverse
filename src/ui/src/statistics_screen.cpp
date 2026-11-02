#include <rv/ui/statistics_screen.hpp>

#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_events.h>

#include <rv/ui/ui_view.hpp>

#include "text_cut.hpp"

namespace rv::ui {
namespace {

/// A line of the body's scrolling, in dp.
constexpr float kLineDp = 48.0f;

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

} // namespace

struct StatisticsScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(statistics::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(statistics::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(statistics::kDataModel));
        }
        modelCreated = true;
        if (auto row = constructor.RegisterStruct<StatisticsGame>()) {
            row.RegisterMember("title", &StatisticsGame::title);
            row.RegisterMember("system", &StatisticsGame::system);
            row.RegisterMember("hours", &StatisticsGame::hours);
            row.RegisterMember("sessions", &StatisticsGame::sessions);
            row.RegisterMember("date", &StatisticsGame::date);
            row.RegisterMember("status", &StatisticsGame::status);
        }
        constructor.RegisterArray<std::vector<StatisticsGame>>();
        if (auto row = constructor.RegisterStruct<StatisticsSystem>()) {
            row.RegisterMember("name", &StatisticsSystem::name);
            row.RegisterMember("hours", &StatisticsSystem::hours);
            row.RegisterMember("sessions", &StatisticsSystem::sessions);
            row.RegisterMember("attempts", &StatisticsSystem::attempts);
            row.RegisterMember("games", &StatisticsSystem::games);
        }
        constructor.RegisterArray<std::vector<StatisticsSystem>>();
        if (auto row = constructor.RegisterStruct<StatisticsStatus>()) {
            row.RegisterMember("status", &StatisticsStatus::status);
            row.RegisterMember("games", &StatisticsStatus::games);
            row.RegisterMember("sessions", &StatisticsStatus::sessions);
            row.RegisterMember("hours", &StatisticsStatus::hours);
        }
        constructor.RegisterArray<std::vector<StatisticsStatus>>();
        constructor.Bind("state", &state);
        constructor.Bind("tab", &tab);
        constructor.Bind("sessions", &content.sessions);
        constructor.Bind("hours", &content.hours);
        constructor.Bind("gamesPlayed", &content.gamesPlayed);
        constructor.Bind("games", &content.games);
        constructor.Bind("crashed", &content.crashed);
        constructor.Bind("leftRunning", &content.leftRunning);
        constructor.Bind("mostPlayed", &content.mostPlayed);
        constructor.Bind("recent", &content.recent);
        constructor.Bind("systems", &content.systems);
        constructor.Bind("statuses", &content.statuses);
        constructor.BindEventCallback("section", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) {
                return;
            }
            if (const auto chosen = statisticsTabFromName(args.front().Get<Rml::String>())) {
                clicked = StatisticsClick{*chosen, false};
            }
        });
        constructor.BindEventCallback("close", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            clicked = StatisticsClick{std::nullopt, true};
        });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(statistics::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the statistics document could not be loaded (see the log)",
                             std::string(statistics::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    [[nodiscard]] Rml::Element* body() const {
        return document != nullptr ? document->GetElementById(Rml::String(statistics::kBodyId)) : nullptr;
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    std::uint64_t boundRevision = 0;
    bool bound = false;
    Rml::String state;
    Rml::String tab;
    StatisticsContent content;
    std::optional<StatisticsClick> clicked;
};

StatisticsScreen::StatisticsScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

StatisticsScreen::~StatisticsScreen() = default;

Expected<std::unique_ptr<StatisticsScreen>> StatisticsScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<StatisticsScreen>(new StatisticsScreen(std::move(impl)));
}

void StatisticsScreen::show(const StatisticsModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    if (!impl.bound || impl.boundRevision != model.revision()) {
        const Rml::String tab(toString(model.tab()));
        const bool otherTab = tab != impl.tab;
        impl.state = Rml::String(toString(model.state()));
        impl.tab = tab;
        impl.content = model.content();
        impl.handle.DirtyAllVariables();
        impl.boundRevision = model.revision();
        impl.bound = true;
        if (otherTab) {
            if (Rml::Element* body = impl.body()) {
                body->SetScrollTop(0.0f);
            }
        }
    }
    if (!impl.shown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.shown = true;
    }
}

void StatisticsScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool StatisticsScreen::visible() const noexcept {
    return m_impl->shown;
}

bool StatisticsScreen::scroll(InfoStep step) {
    Impl& impl = *m_impl;
    Rml::Element* body = impl.shown ? impl.body() : nullptr;
    if (body == nullptr) {
        return false;
    }
    const float top = body->GetScrollTop();
    const float next = scrolledTop(top, step, body->GetClientHeight(), body->GetScrollHeight(),
                                   kLineDp * impl.view.dpRatio());
    if (next == top) {
        return false;
    }
    body->SetScrollTop(next);
    return true;
}

bool StatisticsScreen::handlePointer(const SDL_Event& event) {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr || !isPointer(event.type)) {
        return false;
    }
    static_cast<void>(impl.view.handleEvent(event));
    if (!isPointerButton(event.type)) {
        return false;
    }
    const Rml::Element* hover = impl.view.context().GetHoverElement();
    return hover != nullptr && hover->GetOwnerDocument() == impl.document && hover != impl.document;
}

std::optional<StatisticsClick> StatisticsScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::uint32_t StatisticsScreen::textCut() const {
    Rml::ElementDocument* document = m_impl->document;
    Rml::Element* panel = document != nullptr ? document->GetElementById(Rml::String(statistics::kPanelId)) : nullptr;
    if (panel == nullptr || !m_impl->shown) {
        return 0;
    }
    return detail::elementsWithCutText(*panel);
}

std::string StatisticsScreen::hoursLabel() const {
    Rml::ElementDocument* document = m_impl->document;
    Rml::Element* label =
        document != nullptr ? document->GetElementById(Rml::String(statistics::kHoursLabelId)) : nullptr;
    return label != nullptr && m_impl->shown && label->IsVisible() ? std::string(label->GetInnerRML()) : std::string();
}

} // namespace rv::ui
