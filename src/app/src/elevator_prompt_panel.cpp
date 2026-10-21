#include "elevator_prompt_panel.hpp"

#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include <rv/app/elevator_prompt_ui.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::app {

struct ElevatorPromptPanel::Impl {
    explicit Impl(ui::UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(elevator_prompt::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor =
            view.context().CreateDataModel(Rml::String(elevator_prompt::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(elevator_prompt::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("key", &key);
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(elevator_prompt::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the elevator prompt's document could not be loaded (see the log)",
                             std::string(elevator_prompt::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    ui::UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    Rml::String key;
};

ElevatorPromptPanel::ElevatorPromptPanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

ElevatorPromptPanel::~ElevatorPromptPanel() = default;

Expected<std::unique_ptr<ElevatorPromptPanel>> ElevatorPromptPanel::create(ui::UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<ElevatorPromptPanel>(new ElevatorPromptPanel(std::move(impl)));
}

void ElevatorPromptPanel::show(const std::string& key) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr || (impl.shown && impl.key == key)) {
        return;
    }
    impl.key = Rml::String(key);
    impl.handle.DirtyAllVariables();
    if (!impl.shown) {
        // Shown without taking the focus: the world keeps the input.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PushToBack();
        impl.shown = true;
    }
}

void ElevatorPromptPanel::hide() {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool ElevatorPromptPanel::visible() const noexcept {
    return m_impl->shown;
}

} // namespace rv::app
