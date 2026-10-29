#include <rv/ui/regenerate_preview_screen.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_events.h>

#include <rv/ui/directory_model.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

/// A conflict listed ("conflicts").
struct ConflictRow {
    Rml::String reason;
    Rml::String subject;
    Rml::String title;
};

[[nodiscard]] int count(std::uint32_t value) noexcept {
    return static_cast<int>(std::min<std::uint32_t>(value, 1'000'000'000));
}

[[nodiscard]] bool knownFloor(std::string_view id) noexcept {
    return std::find(directory::kFloorIds.begin(), directory::kFloorIds.end(), id) != directory::kFloorIds.end();
}

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

} // namespace

struct RegeneratePreviewScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(regenerate_preview::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor =
            view.context().CreateDataModel(Rml::String(regenerate_preview::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(regenerate_preview::kDataModel));
        }
        modelCreated = true;
        if (auto conflict = constructor.RegisterStruct<ConflictRow>()) {
            conflict.RegisterMember("reason", &ConflictRow::reason);
            conflict.RegisterMember("subject", &ConflictRow::subject);
            conflict.RegisterMember("title", &ConflictRow::title);
        }
        constructor.RegisterArray<std::vector<ConflictRow>>();
        constructor.Bind("kind", &kind);
        constructor.Bind("state", &state);
        constructor.Bind("failure", &failure);
        constructor.Bind("roomKind", &roomKind);
        constructor.Bind("roomTitle", &roomTitle);
        constructor.Bind("floor", &floor);
        constructor.Bind("floorKnown", &floorKnown);
        constructor.Bind("roomsRegenerated", &roomsRegenerated);
        constructor.Bind("roomsAdded", &roomsAdded);
        constructor.Bind("roomsRemoved", &roomsRemoved);
        constructor.Bind("roomsMoved", &roomsMoved);
        constructor.Bind("gamesMoved", &gamesMoved);
        constructor.Bind("faceOutEntering", &faceOutEntering);
        constructor.Bind("faceOutLeaving", &faceOutLeaving);
        constructor.Bind("newlyPlaced", &newlyPlaced);
        constructor.Bind("newlyUnplaced", &newlyUnplaced);
        constructor.Bind("fixturesAdded", &fixturesAdded);
        constructor.Bind("fixturesRemoved", &fixturesRemoved);
        constructor.Bind("fixturesMoved", &fixturesMoved);
        constructor.Bind("pinsKept", &pinsKept);
        constructor.Bind("conflictCount", &conflictCount);
        constructor.Bind("conflictsMore", &conflictsMore);
        constructor.Bind("conflicts", &conflicts);
        constructor.Bind("overrides", &overrides);
        constructor.Bind("changed", &changed);
        constructor.Bind("undoAction", &undoAction);
        constructor.Bind("undoActionKnown", &undoActionKnown);
        constructor.Bind("undoScopeKind", &undoScopeKind);
        constructor.Bind("undoRoomKind", &undoRoomKind);
        constructor.Bind("undoRoomTitle", &undoRoomTitle);
        constructor.Bind("undoFloor", &undoFloor);
        constructor.Bind("undoFloorKnown", &undoFloorKnown);
        constructor.Bind("undoWhen", &undoWhen);
        constructor.Bind("showRegenerate", &showRegenerate);
        constructor.Bind("showDrop", &showDrop);
        constructor.Bind("showConfirmDrop", &showConfirmDrop);
        constructor.Bind("showUndo", &showUndo);
        constructor.Bind("showBack", &showBack);
        constructor.Bind("showCancel", &showCancel);
        constructor.BindEventCallback(Rml::String(regenerate_preview::kButtonEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          if (const auto button =
                                                  regenerateButtonFromName(args.front().Get<Rml::String>())) {
                                              clicked = *button;
                                          }
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(regenerate_preview::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the preview's document could not be loaded (see the log)",
                             std::string(regenerate_preview::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const RegeneratePreviewModel& model) {
        kind = Rml::String(toString(model.kind()));
        state = Rml::String(toString(model.state()));
        failure = Rml::String(model.failure());
        const RegenerateScopeTitle& scope = model.scope();
        roomKind = Rml::String(scope.roomKind);
        roomTitle = Rml::String(scope.roomTitle);
        floor = Rml::String(scope.floor);
        floorKnown = knownFloor(scope.floor);
        const RegeneratePreviewContent& content = model.content();
        roomsRegenerated = count(content.roomsRegenerated);
        roomsAdded = count(content.roomsAdded);
        roomsRemoved = count(content.roomsRemoved);
        roomsMoved = count(content.roomsMoved);
        gamesMoved = count(content.gamesMoved);
        faceOutEntering = count(content.faceOutEntering);
        faceOutLeaving = count(content.faceOutLeaving);
        newlyPlaced = count(content.newlyPlaced);
        newlyUnplaced = count(content.newlyUnplaced);
        fixturesAdded = count(content.fixturesAdded);
        fixturesRemoved = count(content.fixturesRemoved);
        fixturesMoved = count(content.fixturesMoved);
        pinsKept = count(content.pinsKept);
        conflictCount = count(content.conflictCount);
        conflicts.clear();
        for (const RegenerateConflict& conflict : content.conflicts) {
            conflicts.push_back(
                ConflictRow{Rml::String(conflict.reason), Rml::String(conflict.subject), Rml::String(conflict.title)});
        }
        conflictsMore = std::max(0, conflictCount - static_cast<int>(conflicts.size()));
        overrides = count(content.overrides);
        changed = content.changed;
        undoAction = Rml::String(content.undoAction);
        undoActionKnown = std::find(regenerate_preview::kUndoActions.begin(), regenerate_preview::kUndoActions.end(),
                                    content.undoAction) != regenerate_preview::kUndoActions.end();
        // The undone action's scope: a room ("room:..." or "area:..."), a floor ("floor:<id>"), the world.
        const std::string& undoScope = content.undoScope;
        undoScopeKind = undoScope == "world"              ? "world"
                        : undoScope.starts_with("floor:") ? "floor"
                        : undoScope.empty()               ? ""
                                                          : "room";
        undoRoomKind = Rml::String(content.undoScopeTitle.roomKind);
        undoRoomTitle =
            Rml::String(content.undoScopeTitle.roomTitle.empty() ? undoScope : content.undoScopeTitle.roomTitle);
        undoFloor = Rml::String(content.undoScopeTitle.floor);
        undoFloorKnown = knownFloor(content.undoScopeTitle.floor);
        undoWhen = Rml::String(content.undoWhen);
        const std::vector<RegenerateButton> shown = model.buttons();
        const auto has = [&](RegenerateButton button) {
            return std::find(shown.begin(), shown.end(), button) != shown.end();
        };
        showRegenerate = has(RegenerateButton::Regenerate);
        showDrop = has(RegenerateButton::Drop);
        showConfirmDrop = has(RegenerateButton::ConfirmDrop);
        showUndo = has(RegenerateButton::Undo);
        showBack = has(RegenerateButton::Back);
        showCancel = has(RegenerateButton::Cancel);
        handle.DirtyAllVariables();
    }

    void applyFocus() {
        if (model == nullptr) {
            return;
        }
        const std::string id = model->focusedElement();
        if (id.empty()) {
            return;
        }
        Rml::Element* element = document->GetElementById(Rml::String(id));
        if (element == nullptr || view.context().GetFocusElement() == element) {
            return;
        }
        static_cast<void>(element->Focus(true));
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool documentShown = false;
    const RegeneratePreviewModel* model = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<RegenerateButton> clicked;

    Rml::String kind;
    Rml::String state;
    Rml::String failure;
    Rml::String roomKind;
    Rml::String roomTitle;
    Rml::String floor;
    bool floorKnown = false;
    int roomsRegenerated = 0;
    int roomsAdded = 0;
    int roomsRemoved = 0;
    int roomsMoved = 0;
    int gamesMoved = 0;
    int faceOutEntering = 0;
    int faceOutLeaving = 0;
    int newlyPlaced = 0;
    int newlyUnplaced = 0;
    int fixturesAdded = 0;
    int fixturesRemoved = 0;
    int fixturesMoved = 0;
    int pinsKept = 0;
    int conflictCount = 0;
    int conflictsMore = 0;
    std::vector<ConflictRow> conflicts;
    int overrides = 0;
    bool changed = false;
    Rml::String undoAction;
    bool undoActionKnown = false;
    Rml::String undoScopeKind;
    Rml::String undoRoomKind;
    Rml::String undoRoomTitle;
    Rml::String undoFloor;
    bool undoFloorKnown = false;
    Rml::String undoWhen;
    bool showRegenerate = false;
    bool showDrop = false;
    bool showConfirmDrop = false;
    bool showUndo = false;
    bool showBack = false;
    bool showCancel = false;
};

RegeneratePreviewScreen::RegeneratePreviewScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

RegeneratePreviewScreen::~RegeneratePreviewScreen() = default;

Expected<std::unique_ptr<RegeneratePreviewScreen>> RegeneratePreviewScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<RegeneratePreviewScreen>(new RegeneratePreviewScreen(std::move(impl)));
}

void RegeneratePreviewScreen::show(const RegeneratePreviewModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.model = &model;
    if (impl.boundRevision != model.revision()) {
        impl.bind(model);
        impl.boundRevision = model.revision();
    }
    if (!impl.documentShown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.documentShown = true;
    }
}

void RegeneratePreviewScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.model = nullptr;
    if (!impl.documentShown || impl.document == nullptr) {
        impl.documentShown = false;
        return;
    }
    impl.documentShown = false;
    impl.document->Hide();
    // As the directory's (directory_screen.cpp): the focus never stays on an element of the hidden document.
    if (const Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document && focus != impl.document) {
        static_cast<void>(impl.document->Focus());
    }
}

bool RegeneratePreviewScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void RegeneratePreviewScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (!impl.documentShown || impl.document == nullptr) {
        return;
    }
    impl.applyFocus();
}

bool RegeneratePreviewScreen::handlePointer(const SDL_Event& event) {
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

std::optional<RegenerateButton> RegeneratePreviewScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::string RegeneratePreviewScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

} // namespace rv::ui
