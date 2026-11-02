#include <rv/ui/wizard_screen.hpp>

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

/// A row of the browser as the document reads it.
struct RowView {
    int index = 0;
    Rml::String elementId;
    Rml::String name;
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

[[nodiscard]] bool offers(const std::vector<WizardButton>& buttons, WizardButton button) {
    return std::find(buttons.begin(), buttons.end(), button) != buttons.end();
}

} // namespace

struct WizardScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(wizard::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(wizard::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(wizard::kDataModel));
        }
        modelCreated = true;
        if (auto row = constructor.RegisterStruct<RowView>()) {
            row.RegisterMember("index", &RowView::index);
            row.RegisterMember("elementId", &RowView::elementId);
            row.RegisterMember("name", &RowView::name);
        }
        constructor.RegisterArray<std::vector<RowView>>();
        constructor.Bind("page", &page);
        constructor.Bind("pageNumber", &pageNumber);
        constructor.Bind("resumed", &resumed);
        constructor.Bind("detectionWork", &detectionWork);
        constructor.Bind("problem", &problem);
        constructor.Bind("version", &version);
        constructor.Bind("homePath", &folderPath[0]);
        constructor.Bind("homeState", &folderState[0]);
        constructor.Bind("installPath", &folderPath[1]);
        constructor.Bind("installState", &folderState[1]);
        constructor.Bind("romPath", &folderPath[2]);
        constructor.Bind("romState", &folderState[2]);
        constructor.Bind("mediaPath", &folderPath[3]);
        constructor.Bind("mediaState", &folderState[3]);
        constructor.Bind("previewWork", &previewWork);
        constructor.Bind("systems", &systems);
        constructor.Bind("systemsWithGames", &systemsWithGames);
        constructor.Bind("games", &games);
        constructor.Bind("media", &media);
        constructor.Bind("hasCoverage", &hasCoverage);
        constructor.Bind("coverage", &coverage);
        constructor.Bind("unrecognized", &unrecognized);
        constructor.Bind("errors", &errors);
        constructor.Bind("failure", &failure);
        constructor.Bind("importWork", &importWork);
        constructor.Bind("importPhase", &importPhase);
        constructor.Bind("importDone", &importDone);
        constructor.Bind("importTotal", &importTotal);
        constructor.Bind("importFill", &importFill);
        constructor.Bind("importAdded", &importAdded);
        constructor.Bind("lighting", &lighting);
        constructor.Bind("reduceMotion", &reduceMotion);
        constructor.Bind("worldWork", &worldWork);
        constructor.Bind("emulatorsKnown", &emulatorsKnown);
        constructor.Bind("emulatorsReady", &emulatorsReady);
        constructor.Bind("emulatorsBlocked", &emulatorsBlocked);
        constructor.Bind("showNext", &showNext);
        constructor.Bind("showPrevious", &showPrevious);
        constructor.Bind("showQuit", &showQuit);
        constructor.Bind("showRetry", &showRetry);
        constructor.Bind("showEnter", &showEnter);
        constructor.Bind("browserOpen", &browserOpen);
        constructor.Bind("browserState", &browserState);
        constructor.Bind("browserFailure", &browserFailure);
        constructor.Bind("browserPath", &browserPath);
        constructor.Bind("atDrives", &atDrives);
        constructor.Bind("canUse", &canUse);
        constructor.Bind("browserCount", &browserCount);
        constructor.Bind("browserFrom", &browserFrom);
        constructor.Bind("browserTo", &browserTo);
        constructor.Bind("rows", &rows);
        constructor.BindEventCallback(Rml::String(wizard::kButtonEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          if (const auto button = wizardButtonFromName(args.front().Get<Rml::String>())) {
                                              WizardClick made;
                                              made.button = *button;
                                              if (args.size() > 1) {
                                                  made.direction = args[1].Get<int>(1) < 0 ? -1 : 1;
                                              }
                                              clicked = made;
                                          }
                                      });
        constructor.BindEventCallback(
            "browser", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (args.empty()) {
                    return;
                }
                const Rml::String name = args.front().Get<Rml::String>();
                WizardClick made;
                made.kind = WizardClick::Kind::Browser;
                if (name == "up") {
                    made.browser.kind = FolderBrowserClick::Kind::Up;
                } else if (name == "use") {
                    made.browser.kind = FolderBrowserClick::Kind::Use;
                } else if (name == "cancel") {
                    made.browser.kind = FolderBrowserClick::Kind::Cancel;
                } else {
                    return;
                }
                clicked = made;
            });
        constructor.BindEventCallback("browser_row",
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          const int index = args.empty() ? -1 : args.front().Get<int>(-1);
                                          if (index < 0) {
                                              return;
                                          }
                                          WizardClick made;
                                          made.kind = WizardClick::Kind::Browser;
                                          made.browser.kind = FolderBrowserClick::Kind::Row;
                                          made.browser.index = static_cast<std::size_t>(index);
                                          clicked = made;
                                      });
        constructor.BindEventCallback("browser_wheel",
                                      [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
                                          const float delta = event.GetParameter("wheel_delta_y", 0.0f);
                                          wheel += static_cast<int>(std::lround(delta));
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(wizard::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the wizard's document could not be loaded (see the log)",
                             std::string(wizard::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const WizardModel& shown, const FolderBrowserModel& folders) {
        page = Rml::String(toString(shown.page()));
        pageNumber = static_cast<int>(shown.page()) + 1;
        resumed = shown.resumed();
        detectionWork = Rml::String(toString(shown.detectionWork()));
        const WizardDetection& detection = shown.detection();
        problem = Rml::String(detection.problem);
        version = Rml::String(detection.version);
        for (std::size_t index = 0; index < kWizardFolderCount; ++index) {
            folderPath[index] = Rml::String(detection.folders[index].path);
            folderState[index] = Rml::String(detection.folders[index].state);
        }
        previewWork = Rml::String(toString(shown.previewWork()));
        const WizardPreview& preview = shown.preview();
        systems = asInt(preview.systems);
        systemsWithGames = asInt(preview.systemsWithGames);
        games = asInt(preview.games);
        media = asInt(preview.media);
        hasCoverage = preview.coveragePercent.has_value();
        coverage = preview.coveragePercent.value_or(0);
        unrecognized = asInt(preview.unrecognized);
        errors = asInt(preview.errors);
        failure = Rml::String(shown.failure());
        importWork = Rml::String(toString(shown.importWork()));
        const WizardImport& importing = shown.importing();
        importPhase = Rml::String(importing.phase);
        importDone = asInt(importing.done);
        importTotal = asInt(importing.total);
        importFill = importing.total == 0
                         ? Rml::String("0%")
                         : Rml::CreateString("%d%%", static_cast<int>(100.0 * static_cast<double>(importing.done) /
                                                                      static_cast<double>(importing.total)));
        importAdded = asInt(importing.added);
        lighting = Rml::String(shown.lightingProfile());
        reduceMotion = shown.reduceMotion();
        worldWork = Rml::String(toString(shown.worldWork()));
        emulatorsKnown = shown.emulatorsReady().has_value();
        emulatorsReady = asInt(shown.emulatorsReady().value_or(0));
        emulatorsBlocked = asInt(shown.emulatorsBlocked());
        const std::vector<WizardButton> buttons = shown.buttons();
        showNext = offers(buttons, WizardButton::Next);
        showPrevious = offers(buttons, WizardButton::Previous);
        showQuit = offers(buttons, WizardButton::Quit);
        showRetry = offers(buttons, WizardButton::Retry);
        showEnter = offers(buttons, WizardButton::Enter);

        browserOpen = folders.isOpen();
        browserState = Rml::String(toString(folders.state()));
        browserFailure =
            folders.failure() ? Rml::String(toString(*folders.failure())) : Rml::String();
        browserPath = Rml::String(core::pathToUtf8Lossy(folders.folder()));
        atDrives = folders.atDrives();
        canUse = folders.canUse();
        browserCount = asInt(folders.count());
        browserFrom = asInt(folders.first() + 1);
        browserTo = asInt(std::min(folders.count(), folders.first() + folder_browser::kVisibleRows));
        rows.clear();
        int index = 0;
        for (const FolderRow& row : folders.rows()) {
            rows.push_back(RowView{index, Rml::String(FolderBrowserModel::rowElement(static_cast<std::size_t>(index))),
                                   Rml::String(row.name)});
            ++index;
        }
        handle.DirtyAllVariables();
    }

    void applyFocus() {
        if (model == nullptr || browser == nullptr) {
            return;
        }
        const std::string id = browser->isOpen() ? browser->focusedElement() : model->focusedElement();
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
    const WizardModel* model = nullptr;
    const FolderBrowserModel* browser = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<std::uint64_t> boundBrowser;
    std::optional<WizardClick> clicked;
    int wheel = 0;

    Rml::String page;
    int pageNumber = 1;
    bool resumed = false;
    Rml::String detectionWork;
    Rml::String problem;
    Rml::String version;
    Rml::String folderPath[kWizardFolderCount];
    Rml::String folderState[kWizardFolderCount];
    Rml::String previewWork;
    int systems = 0;
    int systemsWithGames = 0;
    int games = 0;
    int media = 0;
    bool hasCoverage = false;
    int coverage = 0;
    int unrecognized = 0;
    int errors = 0;
    Rml::String failure;
    Rml::String importWork;
    Rml::String importPhase;
    int importDone = 0;
    int importTotal = 0;
    Rml::String importFill;
    int importAdded = 0;
    Rml::String lighting;
    bool reduceMotion = false;
    Rml::String worldWork;
    bool emulatorsKnown = false;
    int emulatorsReady = 0;
    int emulatorsBlocked = 0;
    bool showNext = false;
    bool showPrevious = false;
    bool showQuit = false;
    bool showRetry = false;
    bool showEnter = false;
    bool browserOpen = false;
    Rml::String browserState;
    Rml::String browserFailure;
    Rml::String browserPath;
    bool atDrives = false;
    bool canUse = false;
    int browserCount = 0;
    int browserFrom = 0;
    int browserTo = 0;
    std::vector<RowView> rows;
};

WizardScreen::WizardScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

WizardScreen::~WizardScreen() = default;

Expected<std::unique_ptr<WizardScreen>> WizardScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<WizardScreen>(new WizardScreen(std::move(impl)));
}

void WizardScreen::show(const WizardModel& model, const FolderBrowserModel& browser) {
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

void WizardScreen::hide() {
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

bool WizardScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void WizardScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (impl.documentShown && impl.document != nullptr) {
        impl.applyFocus();
    }
}

bool WizardScreen::handlePointer(const SDL_Event& event) {
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

std::optional<WizardClick> WizardScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

int WizardScreen::takeWheel() noexcept {
    return std::exchange(m_impl->wheel, 0);
}

std::uint32_t WizardScreen::textCut() const {
    Rml::Element* panel =
        m_impl->document != nullptr ? m_impl->document->GetElementById(Rml::String(wizard::kPanelId)) : nullptr;
    return panel != nullptr && panel->IsVisible() ? detail::elementsWithCutText(*panel) : 0;
}

std::uint32_t WizardScreen::browserTextCut() const {
    Rml::Element* panel =
        m_impl->document != nullptr ? m_impl->document->GetElementById(Rml::String(wizard::kBrowserPanelId)) : nullptr;
    return panel != nullptr && panel->IsVisible() ? detail::elementsWithCutText(*panel) : 0;
}

} // namespace rv::ui
