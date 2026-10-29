#include "world_notice_panel.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <spdlog/spdlog.h>

#include <rv/app/world_notice_ui.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::app {

struct WorldNoticePanel::Impl {
    explicit Impl(ui::UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(world_notice::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(world_notice::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(world_notice::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("notice", &notice);
        constructor.Bind("state", &state);
        constructor.Bind("room", &room);
        constructor.Bind("roomKind", &roomKind);
        constructor.Bind("roomTitle", &roomTitle);
        constructor.Bind("roomNumber", &roomNumber);
        constructor.Bind("moreRooms", &moreRooms);
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(world_notice::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the world notice's document could not be loaded (see the log)",
                             std::string(world_notice::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    ui::UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    WorldNoticeContent content;
    Rml::String notice;
    Rml::String state;
    Rml::String room;
    Rml::String roomKind;
    Rml::String roomTitle;
    int roomNumber = 1;
    int moreRooms = 0;
};

WorldNoticePanel::WorldNoticePanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

WorldNoticePanel::~WorldNoticePanel() = default;

Expected<std::unique_ptr<WorldNoticePanel>> WorldNoticePanel::create(ui::UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<WorldNoticePanel>(new WorldNoticePanel(std::move(impl)));
}

void WorldNoticePanel::show(const WorldNoticeContent& content) {
    Impl& impl = *m_impl;
    if (content.notice.empty()) {
        hide();
        return;
    }
    if (impl.document == nullptr || (impl.shown && impl.content == content)) {
        return;
    }
    impl.content = content;
    impl.notice = Rml::String(content.notice);
    impl.state = Rml::String(content.state);
    impl.room = Rml::String(content.startRoom);
    impl.roomKind = Rml::String(content.roomKind);
    impl.roomTitle = Rml::String(content.roomTitle);
    impl.roomNumber = static_cast<int>(std::min<std::uint32_t>(content.roomNumber, 1'000'000));
    impl.moreRooms = static_cast<int>(std::min<std::uint32_t>(content.moreRooms, 1'000'000));
    impl.handle.DirtyAllVariables();
    if (!impl.shown) {
        // Shown without taking the focus: the world and the quick catalog keep theirs.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PushToBack();
        impl.shown = true;
    }
    spdlog::info("World notice: {}", content.notice);
}

void WorldNoticePanel::hide() {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool WorldNoticePanel::visible() const noexcept {
    return m_impl->shown;
}

} // namespace rv::app
