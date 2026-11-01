#include <rv/ui/system_menu_screen.hpp>

#include <string>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_events.h>

#include <rv/ui/ui_view.hpp>

#include "text_cut.hpp"

namespace rv::ui {
namespace {

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

} // namespace

struct SystemMenuScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(system_menu::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(system_menu::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(system_menu::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("view", &viewName);
        constructor.BindEventCallback(Rml::String(system_menu::kButtonEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          if (const auto button =
                                                  systemMenuButtonFromName(args.front().Get<Rml::String>())) {
                                              clicked = *button;
                                          }
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(system_menu::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the menu's document could not be loaded (see the log)",
                             std::string(system_menu::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void applyFocus() {
        if (model == nullptr) {
            return;
        }
        Rml::Element* element = document->GetElementById(Rml::String(model->focusedElement()));
        if (element != nullptr && view.context().GetFocusElement() != element) {
            static_cast<void>(element->Focus(true));
        }
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool documentShown = false;
    const SystemMenuModel* model = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<SystemMenuButton> clicked;
    Rml::String viewName;
};

SystemMenuScreen::SystemMenuScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

SystemMenuScreen::~SystemMenuScreen() = default;

Expected<std::unique_ptr<SystemMenuScreen>> SystemMenuScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<SystemMenuScreen>(new SystemMenuScreen(std::move(impl)));
}

void SystemMenuScreen::show(const SystemMenuModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.model = &model;
    if (impl.boundRevision != model.revision()) {
        impl.viewName = Rml::String(toString(model.view()));
        impl.handle.DirtyAllVariables();
        impl.boundRevision = model.revision();
    }
    if (!impl.documentShown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.documentShown = true;
    }
}

void SystemMenuScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.model = nullptr;
    if (!impl.documentShown || impl.document == nullptr) {
        impl.documentShown = false;
        return;
    }
    impl.documentShown = false;
    impl.document->Hide();
    // As the directory's: the focus never stays on an element of the hidden document.
    if (const Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document && focus != impl.document) {
        static_cast<void>(impl.document->Focus());
    }
}

bool SystemMenuScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void SystemMenuScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (impl.documentShown && impl.document != nullptr) {
        impl.applyFocus();
    }
}

bool SystemMenuScreen::handlePointer(const SDL_Event& event) {
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

std::optional<SystemMenuButton> SystemMenuScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::string SystemMenuScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

std::uint32_t SystemMenuScreen::textCut() const {
    Rml::Element* panel = m_impl->document != nullptr
                              ? m_impl->document->GetElementById(Rml::String(system_menu::kPanelId))
                              : nullptr;
    return panel != nullptr ? detail::elementsWithCutText(*panel) : 0;
}

} // namespace rv::ui
