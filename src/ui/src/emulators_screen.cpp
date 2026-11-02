#include <rv/ui/emulators_screen.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL_events.h>

#include <rv/core/utf8.hpp>
#include <rv/ui/ui_view.hpp>

#include "text_cut.hpp"

namespace rv::ui {
namespace {

/// A system's row as the document reads it.
struct SystemView {
    int index = 0;
    Rml::String elementId;
    Rml::String name;
    Rml::String fullName;
    int games = 0;
    Rml::String readiness;
    Rml::String blocker;
    Rml::String source;
    Rml::String choice;
    bool verified = false;
    bool stored = false;
};

/// An option's row.
struct OptionView {
    int index = 0;
    Rml::String elementId;
    Rml::String name;
    bool profile = false;
    Rml::String readiness;
    Rml::String blocker;
    Rml::String program;
    bool chosen = false;
    bool verified = false;
    bool clear = false;
};

/// A row of the browser.
struct RowView {
    int index = 0;
    Rml::String elementId;
    Rml::String name;
    bool program = false;
};

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

[[nodiscard]] int asInt(std::size_t value) noexcept {
    return static_cast<int>(std::min<std::size_t>(value, 2'000'000'000));
}

[[nodiscard]] bool offers(const std::vector<EmulatorsButton>& buttons, EmulatorsButton button) {
    return std::find(buttons.begin(), buttons.end(), button) != buttons.end();
}

} // namespace

struct EmulatorsScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(emulators::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(emulators::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(emulators::kDataModel));
        }
        modelCreated = true;
        if (auto row = constructor.RegisterStruct<SystemView>()) {
            row.RegisterMember("index", &SystemView::index);
            row.RegisterMember("elementId", &SystemView::elementId);
            row.RegisterMember("name", &SystemView::name);
            row.RegisterMember("fullName", &SystemView::fullName);
            row.RegisterMember("games", &SystemView::games);
            row.RegisterMember("readiness", &SystemView::readiness);
            row.RegisterMember("blocker", &SystemView::blocker);
            row.RegisterMember("source", &SystemView::source);
            row.RegisterMember("choice", &SystemView::choice);
            row.RegisterMember("verified", &SystemView::verified);
            row.RegisterMember("stored", &SystemView::stored);
        }
        constructor.RegisterArray<std::vector<SystemView>>();
        if (auto row = constructor.RegisterStruct<OptionView>()) {
            row.RegisterMember("index", &OptionView::index);
            row.RegisterMember("elementId", &OptionView::elementId);
            row.RegisterMember("name", &OptionView::name);
            row.RegisterMember("profile", &OptionView::profile);
            row.RegisterMember("readiness", &OptionView::readiness);
            row.RegisterMember("blocker", &OptionView::blocker);
            row.RegisterMember("program", &OptionView::program);
            row.RegisterMember("chosen", &OptionView::chosen);
            row.RegisterMember("verified", &OptionView::verified);
            row.RegisterMember("clear", &OptionView::clear);
        }
        constructor.RegisterArray<std::vector<OptionView>>();
        if (auto row = constructor.RegisterStruct<RowView>()) {
            row.RegisterMember("index", &RowView::index);
            row.RegisterMember("elementId", &RowView::elementId);
            row.RegisterMember("name", &RowView::name);
            row.RegisterMember("program", &RowView::program);
        }
        constructor.RegisterArray<std::vector<RowView>>();
        constructor.Bind("state", &state);
        constructor.Bind("viewName", &viewName);
        constructor.Bind("systems", &systems);
        constructor.Bind("options", &options);
        constructor.Bind("current", &current);
        constructor.Bind("chosen", &chosen);
        constructor.Bind("verdict", &verdict);
        constructor.Bind("problem", &problem);
        constructor.Bind("saved", &saved);
        constructor.Bind("sampleTitle", &sampleTitle);
        constructor.Bind("sampleAt", &sampleAt);
        constructor.Bind("sampleCount", &sampleCount);
        constructor.Bind("samplesKnown", &samplesKnown);
        constructor.Bind("showTest", &showTest);
        constructor.Bind("showProgram", &showProgram);
        constructor.Bind("showClearProgram", &showClearProgram);
        constructor.Bind("browserOpen", &browserOpen);
        constructor.Bind("browserState", &browserState);
        constructor.Bind("browserFailure", &browserFailure);
        constructor.Bind("browserPath", &browserPath);
        constructor.Bind("atDrives", &atDrives);
        constructor.Bind("browserCount", &browserCount);
        constructor.Bind("browserFrom", &browserFrom);
        constructor.Bind("browserTo", &browserTo);
        constructor.Bind("rows", &rows);
        constructor.BindEventCallback("system", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            clickScreen(EmulatorsClick::Kind::System, args);
        });
        constructor.BindEventCallback("option", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            clickScreen(EmulatorsClick::Kind::Option, args);
        });
        constructor.BindEventCallback("clear", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            clickScreen(EmulatorsClick::Kind::Clear, args);
        });
        constructor.BindEventCallback("sample", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            clickScreen(EmulatorsClick::Kind::Sample, args);
        });
        constructor.BindEventCallback("button", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) {
                return;
            }
            if (const auto button = emulatorsButtonFromName(args.front().Get<Rml::String>())) {
                EmulatorsScreenClick made;
                made.screen.kind = EmulatorsClick::Kind::Button;
                made.screen.button = *button;
                clicked = made;
            }
        });
        constructor.BindEventCallback("browser", [this](Rml::DataModelHandle, Rml::Event&,
                                                         const Rml::VariantList& args) {
            if (args.empty()) {
                return;
            }
            const Rml::String name = args.front().Get<Rml::String>();
            EmulatorsScreenClick made;
            made.browser = true;
            if (name == "up") {
                made.folder.kind = FolderBrowserClick::Kind::Up;
            } else if (name == "cancel") {
                made.folder.kind = FolderBrowserClick::Kind::Cancel;
            } else {
                return;
            }
            clicked = made;
        });
        constructor.BindEventCallback("browser_row", [this](Rml::DataModelHandle, Rml::Event&,
                                                             const Rml::VariantList& args) {
            const int index = args.empty() ? -1 : args.front().Get<int>(-1);
            if (index < 0) {
                return;
            }
            EmulatorsScreenClick made;
            made.browser = true;
            made.folder.kind = FolderBrowserClick::Kind::Row;
            made.folder.index = static_cast<std::size_t>(index);
            clicked = made;
        });
        constructor.BindEventCallback("browser_wheel", [this](Rml::DataModelHandle, Rml::Event& event,
                                                               const Rml::VariantList&) {
            wheel += static_cast<int>(std::lround(event.GetParameter("wheel_delta_y", 0.0f)));
        });
        handle = constructor.GetModelHandle();
        return ok();
    }

    void clickScreen(EmulatorsClick::Kind kind, const Rml::VariantList& args) {
        EmulatorsScreenClick made;
        made.screen.kind = kind;
        made.screen.index = args.empty() ? 0 : args.front().Get<int>(0);
        clicked = made;
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(emulators::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the emulators document could not be loaded (see the log)",
                             std::string(emulators::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const EmulatorsModel& shown, const FolderBrowserModel& folders) {
        state = Rml::String(toString(shown.state()));
        viewName = Rml::String(toString(shown.view()));
        systems.clear();
        int index = 0;
        for (const EmulatorSystem& system : shown.systems()) {
            systems.push_back(SystemView{index, Rml::String(EmulatorsModel::systemElement(static_cast<std::size_t>(index))),
                                         Rml::String(system.name), Rml::String(system.fullName), asInt(system.games),
                                         Rml::String(system.readiness), Rml::String(system.blocker),
                                         Rml::String(system.source), Rml::String(system.choice), system.verified,
                                         system.stored});
            ++index;
        }
        current = SystemView{};
        options.clear();
        if (const EmulatorSystem* system = shown.system()) {
            current = systems[shown.focusedSystem()];
            index = 0;
            for (const EmulatorOption& option : system->options) {
                options.push_back(OptionView{index, Rml::String(EmulatorsModel::optionElement(static_cast<std::size_t>(index))),
                                             Rml::String(option.name), option.profile, Rml::String(option.readiness),
                                             Rml::String(option.blocker), Rml::String(option.program), option.chosen,
                                             option.verified, false});
                ++index;
            }
            if (shown.offersClear()) {
                OptionView clear;
                clear.index = index;
                clear.elementId = Rml::String(EmulatorsModel::optionElement(static_cast<std::size_t>(index)));
                clear.clear = true;
                options.push_back(std::move(clear));
            }
        }
        chosen = OptionView{};
        if (const EmulatorOption* option = shown.option()) {
            chosen.name = Rml::String(option->name);
            chosen.profile = option->profile;
            chosen.readiness = Rml::String(option->readiness);
            chosen.blocker = Rml::String(option->blocker);
            chosen.program = Rml::String(option->program);
            chosen.chosen = option->chosen;
            chosen.verified = option->verified;
        }
        verdict = Rml::String(shown.verdict());
        problem = Rml::String(shown.problem());
        saved = shown.savedUntested();
        sampleTitle = shown.sample() < shown.samples().size() ? Rml::String(shown.samples()[shown.sample()].title)
                                                              : Rml::String();
        sampleAt = asInt(shown.sample() + 1);
        sampleCount = asInt(shown.samples().size());
        samplesKnown = shown.samplesKnown();
        const std::vector<EmulatorsButton> buttons = shown.buttons();
        showTest = offers(buttons, EmulatorsButton::Test);
        showProgram = offers(buttons, EmulatorsButton::Program);
        showClearProgram = offers(buttons, EmulatorsButton::ClearProgram);

        browserOpen = folders.isOpen();
        browserState = Rml::String(toString(folders.state()));
        browserFailure = folders.failure() ? Rml::String(toString(*folders.failure())) : Rml::String();
        browserPath = Rml::String(core::pathToUtf8Lossy(folders.folder()));
        atDrives = folders.atDrives();
        browserCount = asInt(folders.count());
        browserFrom = asInt(folders.first() + 1);
        browserTo = asInt(std::min(folders.count(), folders.first() + folder_browser::kVisibleRows));
        rows.clear();
        index = 0;
        for (const FolderRow& row : folders.rows()) {
            rows.push_back(RowView{index, Rml::String(FolderBrowserModel::rowElement(static_cast<std::size_t>(index))),
                                   Rml::String(row.name), row.program});
            ++index;
        }
        handle.DirtyAllVariables();
    }

    void applyFocus() {
        if (model == nullptr || browser == nullptr) {
            return;
        }
        const std::string id = browser->isOpen() ? browser->focusedElement() : model->focusedElement();
        if (id.empty()) {
            return;
        }
        Rml::Element* element = document->GetElementById(Rml::String(id));
        if (element == nullptr || view.context().GetFocusElement() == element) {
            return;
        }
        if (element->Focus(true)) {
            element->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        }
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool documentShown = false;
    const EmulatorsModel* model = nullptr;
    const FolderBrowserModel* browser = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<std::uint64_t> boundBrowser;
    std::optional<EmulatorsScreenClick> clicked;
    int wheel = 0;

    Rml::String state;
    Rml::String viewName;
    std::vector<SystemView> systems;
    std::vector<OptionView> options;
    SystemView current;
    OptionView chosen;
    Rml::String verdict;
    Rml::String problem;
    bool saved = false;
    Rml::String sampleTitle;
    int sampleAt = 0;
    int sampleCount = 0;
    bool samplesKnown = false;
    bool showTest = false;
    bool showProgram = false;
    bool showClearProgram = false;
    bool browserOpen = false;
    Rml::String browserState;
    Rml::String browserFailure;
    Rml::String browserPath;
    bool atDrives = false;
    int browserCount = 0;
    int browserFrom = 0;
    int browserTo = 0;
    std::vector<RowView> rows;
};

EmulatorsScreen::EmulatorsScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

EmulatorsScreen::~EmulatorsScreen() = default;

Expected<std::unique_ptr<EmulatorsScreen>> EmulatorsScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<EmulatorsScreen>(new EmulatorsScreen(std::move(impl)));
}

void EmulatorsScreen::show(const EmulatorsModel& model, const FolderBrowserModel& browser) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.model = &model;
    impl.browser = &browser;
    if (impl.boundRevision != model.revision() || impl.boundBrowser != browser.revision()) {
        impl.bind(model, browser);
        impl.boundRevision = model.revision();
        impl.boundBrowser = browser.revision();
    }
    if (!impl.documentShown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.documentShown = true;
    }
}

void EmulatorsScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.wheel = 0;
    impl.model = nullptr;
    impl.browser = nullptr;
    if (!impl.documentShown || impl.document == nullptr) {
        impl.documentShown = false;
        return;
    }
    impl.documentShown = false;
    impl.document->Hide();
    if (const Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document && focus != impl.document) {
        static_cast<void>(impl.document->Focus());
    }
}

bool EmulatorsScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void EmulatorsScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (impl.documentShown && impl.document != nullptr) {
        impl.applyFocus();
    }
}

bool EmulatorsScreen::handlePointer(const SDL_Event& event) {
    Impl& impl = *m_impl;
    if (!impl.documentShown || impl.document == nullptr || !isPointer(event.type)) {
        return false;
    }
    static_cast<void>(impl.view.handleEvent(event));
    if (!isPointerButton(event.type)) {
        return false;
    }
    const Rml::Element* hover = impl.view.context().GetHoverElement();
    return hover != nullptr && hover->GetOwnerDocument() == impl.document && hover != impl.document;
}

std::optional<EmulatorsScreenClick> EmulatorsScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

int EmulatorsScreen::takeWheel() noexcept {
    return std::exchange(m_impl->wheel, 0);
}

std::string EmulatorsScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

std::uint32_t EmulatorsScreen::textCut() const {
    Rml::Element* panel =
        m_impl->document != nullptr ? m_impl->document->GetElementById(Rml::String(emulators::kPanelId)) : nullptr;
    return panel != nullptr && panel->IsVisible() ? detail::elementsWithCutText(*panel) : 0;
}

std::uint32_t EmulatorsScreen::browserTextCut() const {
    Rml::Element* panel = m_impl->document != nullptr
                              ? m_impl->document->GetElementById(Rml::String(emulators::kBrowserPanelId))
                              : nullptr;
    return panel != nullptr && panel->IsVisible() ? detail::elementsWithCutText(*panel) : 0;
}

} // namespace rv::ui
