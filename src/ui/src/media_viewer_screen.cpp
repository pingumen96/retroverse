#include <rv/ui/media_viewer_screen.hpp>

#include <cmath>
#include <string>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <fmt/format.h>

#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

/// The data model's "viewer" (media_viewer.rml).
struct ViewerData {
    Rml::String title;
    Rml::String platform;
    /// The page's kind (media_viewer::kKinds) and its 1-based position among `count` pages.
    Rml::String kind;
    int position = 0;
    int count = 0;
    /// toString(PictureState) of the page; "" without pages.
    Rml::String state;
    /// The picture's fitted size, "<n>dp".
    Rml::String width;
    Rml::String height;
    bool hasPrevious = false;
    bool hasNext = false;

    bool operator==(const ViewerData&) const = default;
};

[[nodiscard]] Rml::String dp(float value) {
    return Rml::String(fmt::format("{:.1f}dp", value));
}

} // namespace

std::string_view toString(ViewerClick click) noexcept {
    switch (click) {
    case ViewerClick::Previous:
        return "previous";
    case ViewerClick::Next:
        return "next";
    case ViewerClick::Close:
        return "close";
    }
    return "unknown";
}

std::optional<ViewerClick> viewerClickFromName(std::string_view name) noexcept {
    for (const ViewerClick click : {ViewerClick::Previous, ViewerClick::Next, ViewerClick::Close}) {
        if (toString(click) == name) {
            return click;
        }
    }
    return std::nullopt;
}

struct MediaViewerScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(media_viewer::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(media_viewer::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(media_viewer::kDataModel));
        }
        modelCreated = true;
        if (auto viewer = constructor.RegisterStruct<ViewerData>()) {
            viewer.RegisterMember("title", &ViewerData::title);
            viewer.RegisterMember("platform", &ViewerData::platform);
            viewer.RegisterMember("kind", &ViewerData::kind);
            viewer.RegisterMember("position", &ViewerData::position);
            viewer.RegisterMember("count", &ViewerData::count);
            viewer.RegisterMember("state", &ViewerData::state);
            viewer.RegisterMember("width", &ViewerData::width);
            viewer.RegisterMember("height", &ViewerData::height);
            viewer.RegisterMember("hasPrevious", &ViewerData::hasPrevious);
            viewer.RegisterMember("hasNext", &ViewerData::hasNext);
        }
        constructor.Bind("viewer", &data);
        constructor.BindEventCallback(Rml::String(media_viewer::kPageEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (!args.empty()) {
                                              clicked = viewerClickFromName(args.front().Get<Rml::String>());
                                          }
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(media_viewer::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the media viewer's document could not be loaded (see the log)",
                             std::string(media_viewer::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    /// The model's page as the document binds it, the picture fitted to the stage last laid out (in dp).
    [[nodiscard]] ViewerData dataOf(const MediaViewerModel& model) const {
        ViewerData next;
        next.title = Rml::String(model.title());
        next.platform = Rml::String(model.platform());
        next.count = static_cast<int>(model.pages().size());
        if (const MediaPage* page = model.current()) {
            next.kind = Rml::String(page->kind);
            next.position = static_cast<int>(model.index()) + 1;
            next.state = Rml::String(toString(page->state));
            next.hasPrevious = model.index() > 0;
            next.hasNext = model.index() + 1 < model.pages().size();
            const float ratio = view.dpRatio() > 0.0f ? view.dpRatio() : 1.0f;
            const PictureFit fit = fitPicture(stage.x / ratio, stage.y / ratio, page->width, page->height);
            next.width = dp(std::floor(fit.width));
            next.height = dp(std::floor(fit.height));
        }
        return next;
    }

    void bind(const ViewerData& next) {
        if (next == data) {
            return;
        }
        data = next;
        handle.DirtyAllVariables();
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    ViewerData data;
    /// The model last shown (afterUpdate fits its picture again when the stage changes) and the stage's size in pixels.
    const MediaViewerModel* model = nullptr;
    glm::vec2 stage{0.0f, 0.0f};
    std::optional<ViewerClick> clicked;
};

MediaViewerScreen::MediaViewerScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

MediaViewerScreen::~MediaViewerScreen() = default;

Expected<std::unique_ptr<MediaViewerScreen>> MediaViewerScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<MediaViewerScreen>(new MediaViewerScreen(std::move(impl)));
}

void MediaViewerScreen::show(const MediaViewerModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.model = &model;
    impl.bind(impl.dataOf(model));
    if (!impl.shown) {
        // Over every other document, without taking the focus from the screen under it (the application keeps the
        // input).
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.shown = true;
    }
}

void MediaViewerScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.model = nullptr;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool MediaViewerScreen::visible() const noexcept {
    return m_impl->shown;
}

void MediaViewerScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return;
    }
    const std::optional<glm::vec4> box = impl.view.elementBox(media_viewer::kStageId);
    const glm::vec2 stage = box ? glm::vec2(box->z, box->w) : glm::vec2(0.0f);
    if (stage != impl.stage) {
        impl.stage = stage;
        if (impl.model != nullptr) {
            impl.bind(impl.dataOf(*impl.model));
        }
    }
}

std::optional<ViewerClick> MediaViewerScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::optional<glm::vec4> MediaViewerScreen::pictureBox() const {
    const Impl& impl = *m_impl;
    if (!impl.shown || impl.data.state != Rml::String(toString(PictureState::Shown))) {
        return std::nullopt;
    }
    return impl.view.elementBox(media_viewer::kPictureId);
}

std::optional<glm::vec4> MediaViewerScreen::stageBox() const {
    if (!m_impl->shown) {
        return std::nullopt;
    }
    return m_impl->view.elementBox(media_viewer::kStageId);
}

} // namespace rv::ui
