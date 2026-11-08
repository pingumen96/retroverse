#include "plaque_card_panel.hpp"

#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <rv/ui/ui_view.hpp>

namespace rv::app {

struct PlaqueCardPanel::Impl {
    explicit Impl(ui::UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(room_plaque::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(room_plaque::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(room_plaque::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("name", &name);
        constructor.Bind("maker", &maker);
        constructor.Bind("year", &year);
        constructor.Bind("type", &type);
        constructor.Bind("description", &description);
        constructor.Bind("logo", &logo);
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(room_plaque::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the plaque's card could not be loaded (see the log)",
                             std::string(room_plaque::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    ui::UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    PlaqueCardContent content;
    Rml::String name;
    Rml::String maker;
    Rml::String year;
    Rml::String type;
    Rml::String description;
    Rml::String logo;
};

PlaqueCardPanel::PlaqueCardPanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

PlaqueCardPanel::~PlaqueCardPanel() = default;

Expected<std::unique_ptr<PlaqueCardPanel>> PlaqueCardPanel::create(ui::UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<PlaqueCardPanel>(new PlaqueCardPanel(std::move(impl)));
}

void PlaqueCardPanel::show(const PlaqueCardContent& content) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.content = content;
    impl.name = Rml::String(content.name);
    impl.maker = Rml::String(content.maker);
    impl.year = Rml::String(content.year);
    impl.type = Rml::String(content.type);
    impl.description = Rml::String(content.description);
    impl.logo = Rml::String(content.logo);
    impl.handle.DirtyAllVariables();
    if (!impl.shown) {
        // Shown without taking the focus: the application keeps the input.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.shown = true;
    }
    if (Rml::Element* body = impl.document->GetElementById(Rml::String(room_plaque::kBodyRegionId)); body != nullptr) {
        body->SetScrollTop(0.0f);
    }
}

void PlaqueCardPanel::scroll(int steps) {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr) {
        return;
    }
    if (Rml::Element* body = impl.document->GetElementById(Rml::String(room_plaque::kBodyRegionId)); body != nullptr) {
        constexpr float kLineDp = 40.0f;
        const float line = kLineDp * impl.view.context().GetDensityIndependentPixelRatio();
        body->SetScrollTop(body->GetScrollTop() + static_cast<float>(steps) * line);
    }
}

void PlaqueCardPanel::hide() {
    Impl& impl = *m_impl;
    if (impl.shown && impl.document != nullptr) {
        impl.document->Hide();
    }
    impl.shown = false;
}

bool PlaqueCardPanel::visible() const noexcept {
    return m_impl->shown;
}

const PlaqueCardContent& PlaqueCardPanel::content() const noexcept {
    return m_impl->content;
}

} // namespace rv::app
