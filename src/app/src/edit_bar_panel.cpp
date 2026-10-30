#include "edit_bar_panel.hpp"

#include <algorithm>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include <rv/ui/ui_view.hpp>

namespace rv::app {

struct EditBarPanel::Impl {
    explicit Impl(ui::UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(edit_bar::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(edit_bar::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(edit_bar::kDataModel));
        }
        modelCreated = true;
        constructor.Bind("mode", &mode);
        constructor.Bind("roomTitle", &roomTitle);
        constructor.Bind("roomKind", &roomKind);
        constructor.Bind("roomNumber", &roomNumber);
        constructor.Bind("target", &target);
        constructor.Bind("targetTitle", &targetTitle);
        constructor.Bind("targetPinned", &targetPinned);
        constructor.Bind("targetFaceOut", &targetFaceOut);
        constructor.Bind("targetFree", &targetFree);
        constructor.Bind("targetOwn", &targetOwn);
        constructor.Bind("holding", &holding);
        constructor.Bind("heldTitle", &heldTitle);
        constructor.Bind("ending", &ending);
        constructor.Bind("pending", &pending);
        constructor.Bind("refusal", &refusal);
        constructor.Bind("refusalKnown", &refusalKnown);
        constructor.Bind("saveRefused", &saveRefused);
        constructor.Bind("menuFocus", &menuFocus);
        constructor.Bind("device", &device);
        constructor.Bind("fixtureKind", &fixtureKind);
        constructor.Bind("fixtureHung", &fixtureHung);
        constructor.Bind("fixtureLocked", &fixtureLocked);
        constructor.Bind("placeMoved", &placeMoved);
        constructor.Bind("verdict", &verdict);
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(edit_bar::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the editor's bar's document could not be loaded (see the log)",
                             std::string(edit_bar::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const EditBarContent& value) {
        mode = Rml::String(value.mode);
        roomTitle = Rml::String(value.roomTitle);
        roomKind = Rml::String(value.roomKind);
        roomNumber = static_cast<int>(std::min<std::uint32_t>(value.roomNumber, 1'000'000));
        target = Rml::String(value.target);
        targetTitle = Rml::String(value.targetTitle);
        targetPinned = value.targetPinned;
        targetFaceOut = value.targetFaceOut;
        targetFree = value.target == "slot" || value.target == "dressed";
        targetOwn = value.targetOwn;
        holding = value.holding;
        heldTitle = Rml::String(value.heldTitle);
        ending = Rml::String(value.ending);
        pending = static_cast<int>(std::min<std::uint32_t>(value.pending, 1'000'000));
        refusal = Rml::String(value.refusal);
        refusalKnown = value.refusal.empty() || std::find(edit_bar::kRules.begin(), edit_bar::kRules.end(),
                                                          value.refusal) != edit_bar::kRules.end();
        saveRefused = value.saveRefused;
        menuFocus = Rml::String(value.menuFocus);
        device = Rml::String(value.device);
        fixtureKind = Rml::String(value.fixtureKind);
        fixtureHung = value.fixtureHung;
        fixtureLocked = value.fixtureLocked;
        placeMoved = value.placeMoved;
        verdict = Rml::String(value.verdict);
        handle.DirtyAllVariables();
    }

    ui::UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    EditBarContent content;
    Rml::String mode;
    Rml::String roomTitle;
    Rml::String roomKind;
    int roomNumber = 1;
    Rml::String target;
    Rml::String targetTitle;
    bool targetPinned = false;
    bool targetFaceOut = false;
    bool targetFree = false;
    bool targetOwn = false;
    bool holding = false;
    Rml::String heldTitle;
    Rml::String ending;
    int pending = 0;
    Rml::String refusal;
    bool refusalKnown = true;
    bool saveRefused = false;
    Rml::String menuFocus;
    Rml::String device;
    Rml::String fixtureKind;
    bool fixtureHung = false;
    bool fixtureLocked = false;
    bool placeMoved = false;
    Rml::String verdict;
};

EditBarPanel::EditBarPanel(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

EditBarPanel::~EditBarPanel() = default;

Expected<std::unique_ptr<EditBarPanel>> EditBarPanel::create(ui::UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<EditBarPanel>(new EditBarPanel(std::move(impl)));
}

void EditBarPanel::show(const EditBarContent& content) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr || (impl.shown && impl.content == content)) {
        return;
    }
    impl.content = content;
    impl.bind(content);
    if (!impl.shown) {
        // Shown without taking the focus: the world keeps it.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PushToBack();
        impl.shown = true;
    }
}

void EditBarPanel::hide() {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool EditBarPanel::visible() const noexcept {
    return m_impl->shown;
}

} // namespace rv::app
