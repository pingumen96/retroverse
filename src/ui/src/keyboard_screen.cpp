#include <rv/ui/keyboard_screen.hpp>

#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>

#include <fmt/format.h>

#include <rv/ui/ui_view.hpp>

#include "text_cut.hpp"

namespace rv::ui {
namespace {

/// A key as the document binds it: its place in percent of the grid, fixed from the table.
struct KeyData {
    Rml::String id;
    Rml::String kind;
    Rml::String label;
    Rml::String left;
    Rml::String top;
    Rml::String width;
    Rml::String height;
    int index = 0;
};

[[nodiscard]] Rml::String percent(int part, int whole) {
    return fmt::format("{:.4f}%", 100.0 * part / whole);
}

} // namespace

struct KeyboardScreen::Impl {
    explicit Impl(UiView& view) : view(view) {
        const std::span<const KeyboardKey> table = keyboardKeys();
        keys.reserve(table.size());
        for (std::size_t index = 0; index < table.size(); ++index) {
            const KeyboardKey& key = table[index];
            keys.push_back(KeyData{fmt::format("kb-{}", index), Rml::String(toString(key.kind)), Rml::String(key.text),
                                   percent(key.column, keyboard::kColumns), percent(key.row, keyboard::kRows),
                                   percent(key.span, keyboard::kColumns), percent(1, keyboard::kRows),
                                   static_cast<int>(index)});
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(keyboard::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(keyboard::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(keyboard::kDataModel));
        }
        modelCreated = true;
        if (auto key = constructor.RegisterStruct<KeyData>()) {
            key.RegisterMember("id", &KeyData::id);
            key.RegisterMember("kind", &KeyData::kind);
            key.RegisterMember("label", &KeyData::label);
            key.RegisterMember("left", &KeyData::left);
            key.RegisterMember("top", &KeyData::top);
            key.RegisterMember("width", &KeyData::width);
            key.RegisterMember("height", &KeyData::height);
            key.RegisterMember("index", &KeyData::index);
        }
        constructor.RegisterArray<std::vector<KeyData>>();
        constructor.Bind("keys", &keys);
        constructor.Bind("selected", &selected);
        constructor.BindEventCallback(Rml::String(keyboard::kPressEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (!args.empty()) {
                                              clicked = KeyboardClick{
                                                  static_cast<std::size_t>(args.front().Get<int>())};
                                          }
                                      });
        constructor.BindEventCallback(
            Rml::String(keyboard::kOutsideEvent),
            [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { clicked = KeyboardClick{}; });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(keyboard::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the keyboard's document could not be loaded (see the log)",
                             std::string(keyboard::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool documentShown = false;
    std::optional<std::uint64_t> boundRevision;
    std::optional<KeyboardClick> clicked;
    std::vector<KeyData> keys;
    int selected = 0;
};

KeyboardScreen::KeyboardScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

KeyboardScreen::~KeyboardScreen() = default;

Expected<std::unique_ptr<KeyboardScreen>> KeyboardScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<KeyboardScreen>(new KeyboardScreen(std::move(impl)));
}

void KeyboardScreen::show(const KeyboardModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    if (impl.boundRevision != model.revision()) {
        impl.selected = static_cast<int>(model.selected());
        impl.handle.DirtyVariable("selected");
        impl.boundRevision = model.revision();
    }
    if (!impl.documentShown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.documentShown = true;
    }
}

void KeyboardScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    if (!impl.documentShown || impl.document == nullptr) {
        impl.documentShown = false;
        return;
    }
    impl.documentShown = false;
    impl.document->Hide();
}

bool KeyboardScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void KeyboardScreen::raise() {
    if (m_impl->documentShown && m_impl->document != nullptr) {
        m_impl->document->PullToFront();
    }
}

std::optional<KeyboardClick> KeyboardScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::uint32_t KeyboardScreen::textCut() const {
    Rml::Element* panel =
        m_impl->document != nullptr ? m_impl->document->GetElementById(Rml::String(keyboard::kPanelId)) : nullptr;
    return panel != nullptr ? detail::elementsWithCutText(*panel) : 0;
}

std::optional<glm::vec4> KeyboardScreen::gridBox() const {
    return m_impl->view.elementBox(keyboard::kGridId);
}

} // namespace rv::ui
