#include <rv/ui/controller_notice_screen.hpp>

#include <string>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

constexpr std::string_view kDocumentPath = "ui/controller_notice.rml";

} // namespace

struct ControllerNoticeScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
};

ControllerNoticeScreen::ControllerNoticeScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

ControllerNoticeScreen::~ControllerNoticeScreen() = default;

Expected<std::unique_ptr<ControllerNoticeScreen>> ControllerNoticeScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    impl->document = view.context().LoadDocument(Rml::String(kDocumentPath));
    if (impl->document == nullptr) {
        return makeError(ErrorCode::IoError, "the controller's notice could not be loaded (see the log)",
                         std::string(kDocumentPath));
    }
    impl->document->Hide();
    return std::unique_ptr<ControllerNoticeScreen>(new ControllerNoticeScreen(std::move(impl)));
}

void ControllerNoticeScreen::setShown(bool shown) {
    Impl& impl = *m_impl;
    if (shown == impl.shown) {
        return;
    }
    impl.shown = shown;
    if (shown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
    } else {
        impl.document->Hide();
    }
}

} // namespace rv::ui
