#include <rv/ui/settings_screen.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_events.h>

#include <rv/ui/ui_view.hpp>

#include "text_cut.hpp"

namespace rv::ui {
namespace {

/// A row as the document reads it: its key and its value's key pick the document's words.
struct RowView {
    int index = 0;
    Rml::String elementId;
    Rml::String key;
    Rml::String kind;
    Rml::String value;
    Rml::String text;
    Rml::String fill;
    Rml::String note;
    bool scene = false;
    bool overridden = false;
};

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

[[nodiscard]] Rml::String kindName(SettingsRowKind kind) {
    switch (kind) {
    case SettingsRowKind::Toggle:
        return "toggle";
    case SettingsRowKind::Choice:
        return "choice";
    case SettingsRowKind::Slider:
        return "slider";
    }
    return {};
}

} // namespace

struct SettingsScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(settings::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(settings::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(settings::kDataModel));
        }
        modelCreated = true;
        if (auto row = constructor.RegisterStruct<RowView>()) {
            row.RegisterMember("index", &RowView::index);
            row.RegisterMember("elementId", &RowView::elementId);
            row.RegisterMember("key", &RowView::key);
            row.RegisterMember("kind", &RowView::kind);
            row.RegisterMember("value", &RowView::value);
            row.RegisterMember("text", &RowView::text);
            row.RegisterMember("fill", &RowView::fill);
            row.RegisterMember("note", &RowView::note);
            row.RegisterMember("scene", &RowView::scene);
            row.RegisterMember("overridden", &RowView::overridden);
        }
        constructor.RegisterArray<std::vector<RowView>>();
        constructor.Bind("group", &group);
        constructor.Bind("rows", &rows);
        constructor.Bind("confirmAll", &confirmAll);
        constructor.Bind("device", &device);
        const auto click = [this](SettingsClick::Kind kind) {
            return [this, kind](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                SettingsClick made;
                made.kind = kind;
                if (!args.empty()) {
                    const int index = args.front().Get<int>(-1);
                    if (index < 0) {
                        return;
                    }
                    made.index = static_cast<std::size_t>(index);
                }
                if (args.size() > 1) {
                    made.direction = args[1].Get<int>(1) < 0 ? -1 : 1;
                }
                clicked = made;
            };
        };
        constructor.BindEventCallback(Rml::String(settings::kRowEvent), click(SettingsClick::Kind::Row));
        constructor.BindEventCallback(Rml::String(settings::kStepEvent), click(SettingsClick::Kind::Step));
        constructor.BindEventCallback(Rml::String(settings::kRestoreEvent), click(SettingsClick::Kind::Restore));
        constructor.BindEventCallback(Rml::String(settings::kRestoreAllEvent), click(SettingsClick::Kind::RestoreAll));
        constructor.BindEventCallback(Rml::String(settings::kTabEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          const Rml::String name = args.front().Get<Rml::String>();
                                          const auto found = std::find(settings::kGroups.begin(),
                                                                       settings::kGroups.end(), std::string_view(name));
                                          if (found != settings::kGroups.end()) {
                                              SettingsClick made;
                                              made.kind = SettingsClick::Kind::Tab;
                                              made.index =
                                                  static_cast<std::size_t>(found - settings::kGroups.begin());
                                              clicked = made;
                                          }
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(settings::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the settings' document could not be loaded (see the log)",
                             std::string(settings::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const SettingsModel& shown) {
        group = Rml::String(shown.group());
        rows.clear();
        int index = 0;
        for (const SettingsRow* row : shown.shown()) {
            RowView made;
            made.index = index;
            made.elementId = Rml::String(SettingsModel::rowElement(static_cast<std::size_t>(index)));
            made.key = Rml::String(row->key);
            made.kind = kindName(row->kind);
            made.value = Rml::String(settingsValueKey(*row));
            made.text = Rml::String(settingsValueText(*row));
            made.fill = Rml::String(settingsFill(*row));
            made.note = Rml::String(row->note);
            made.scene = row->kind == SettingsRowKind::Slider && row->value.is_null();
            made.overridden = row->overridden;
            rows.push_back(std::move(made));
            ++index;
        }
        confirmAll = shown.confirmingRestoreAll();
        device = shown.padDevice() ? "pad" : "keys";
        handle.DirtyAllVariables();
    }

    void applyFocus() {
        if (model == nullptr) {
            return;
        }
        Rml::Element* element = document->GetElementById(Rml::String(model->focusedElement()));
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
    const SettingsModel* model = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<SettingsClick> clicked;

    Rml::String group;
    std::vector<RowView> rows;
    bool confirmAll = false;
    Rml::String device;
};

SettingsScreen::SettingsScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

SettingsScreen::~SettingsScreen() = default;

Expected<std::unique_ptr<SettingsScreen>> SettingsScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<SettingsScreen>(new SettingsScreen(std::move(impl)));
}

void SettingsScreen::show(const SettingsModel& model) {
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

void SettingsScreen::hide() {
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

bool SettingsScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void SettingsScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (impl.documentShown && impl.document != nullptr) {
        impl.applyFocus();
    }
}

bool SettingsScreen::handlePointer(const SDL_Event& event) {
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

std::optional<SettingsClick> SettingsScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::string SettingsScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

std::uint32_t SettingsScreen::textCut() const {
    Rml::Element* panel =
        m_impl->document != nullptr ? m_impl->document->GetElementById(Rml::String(settings::kPanelId)) : nullptr;
    return panel != nullptr ? detail::elementsWithCutText(*panel) : 0;
}

} // namespace rv::ui
