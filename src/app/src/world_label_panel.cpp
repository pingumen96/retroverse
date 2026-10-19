#include "world_label_panel.hpp"

#include <cmath>
#include <utility>

#include <fmt/format.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include <rv/ui/ui_view.hpp>

namespace rv::app {
namespace {

/// A length in whole pixels (the label moves by whole pixels, so its text never shimmers between two).
[[nodiscard]] Rml::String pixels(float value) {
    return Rml::String(fmt::format("{}px", static_cast<long>(std::lround(value))));
}

} // namespace

struct WorldLabelPanel::Impl {
    explicit Impl(ui::UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(world_label::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(world_label::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(world_label::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("title", &title);
        constructor.Bind("available", &available);
        constructor.Bind("left", &left);
        constructor.Bind("bottom", &bottom);
        constructor.Bind("width", &width);
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(world_label::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the hover label's document could not be loaded (see the log)",
                             std::string(world_label::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    ui::UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    WorldLabelContent content;
    glm::ivec2 viewSize{0};
    Rml::String title;
    bool available = true;
    Rml::String left;
    Rml::String bottom;
    Rml::String width;
};

WorldLabelPanel::WorldLabelPanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

WorldLabelPanel::~WorldLabelPanel() = default;

Expected<std::unique_ptr<WorldLabelPanel>> WorldLabelPanel::create(ui::UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<WorldLabelPanel>(new WorldLabelPanel(std::move(impl)));
}

void WorldLabelPanel::show(const WorldLabelContent& content, glm::ivec2 viewSize) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr || (impl.shown && impl.content == content && impl.viewSize == viewSize)) {
        return;
    }
    impl.content = content;
    impl.viewSize = viewSize;
    impl.title = Rml::String(content.title);
    impl.available = content.available;
    // A band as wide as the view, centred on the anchor: the title, centred in it, stands over the item whatever its
    // length (a band partly outside the view is clipped like any element).
    const float bandWidth = static_cast<float>(viewSize.x);
    impl.left = pixels(content.x - 0.5f * bandWidth);
    impl.width = pixels(bandWidth);
    impl.bottom = pixels(static_cast<float>(viewSize.y) - content.y + world_label::kGapPixels);
    impl.handle.DirtyAllVariables();
    if (!impl.shown) {
        // Shown without taking the focus: the world and the quick catalog keep theirs.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PushToBack();
        impl.shown = true;
    }
}

void WorldLabelPanel::hide() {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool WorldLabelPanel::visible() const noexcept {
    return m_impl->shown;
}

} // namespace rv::app
