#include <rv/ui/game_card_screen.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_events.h>

#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

/// The data model's "card" (game_card.rml).
struct CardData {
    Rml::String title;
    Rml::String platform;
    /// "" when unknown.
    Rml::String year;
    Rml::String developer;
    Rml::String genre;
    Rml::String players;
    Rml::String state;
    Rml::String readiness;
    Rml::String blocker;
    bool verified = false;
    bool details = false;
    bool unavailable = false;
    int otherVersions = 0;
    bool cover = false;
    /// The cover's box in its frame (cardCoverBox()) as CSS percentages: its width and height, its left and top
    /// margins (of the frame's width, as CSS takes a margin's percentage).
    Rml::String coverWidth = "100%";
    Rml::String coverHeight = "100%";
    Rml::String coverLeft = "0%";
    Rml::String coverTop = "0%";
    Rml::String outcome;
    /// The outcome has a sentence of its own (game_card::kOutcomeCodes): else the general one.
    bool outcomeKnown = false;
};

/// `fraction` as a CSS percentage ("73.4375%"), to a ten-thousandth of a percent.
[[nodiscard]] Rml::String percent(double fraction) {
    std::array<char, 32> text{};
    const auto [end, error] =
        std::to_chars(text.data(), text.data() + text.size() - 1, fraction * 100.0, std::chars_format::fixed, 4);
    if (error != std::errc()) {
        return "100%";
    }
    *end = '%';
    return Rml::String(text.data(), static_cast<std::size_t>(end - text.data()) + 1);
}

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

} // namespace

struct GameCardScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(game_card::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(game_card::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(game_card::kDataModel));
        }
        modelCreated = true;
        if (auto card = constructor.RegisterStruct<CardData>()) {
            card.RegisterMember("title", &CardData::title);
            card.RegisterMember("platform", &CardData::platform);
            card.RegisterMember("year", &CardData::year);
            card.RegisterMember("developer", &CardData::developer);
            card.RegisterMember("genre", &CardData::genre);
            card.RegisterMember("players", &CardData::players);
            card.RegisterMember("state", &CardData::state);
            card.RegisterMember("readiness", &CardData::readiness);
            card.RegisterMember("blocker", &CardData::blocker);
            card.RegisterMember("verified", &CardData::verified);
            card.RegisterMember("details", &CardData::details);
            card.RegisterMember("unavailable", &CardData::unavailable);
            card.RegisterMember("otherVersions", &CardData::otherVersions);
            card.RegisterMember("cover", &CardData::cover);
            card.RegisterMember("coverWidth", &CardData::coverWidth);
            card.RegisterMember("coverHeight", &CardData::coverHeight);
            card.RegisterMember("coverLeft", &CardData::coverLeft);
            card.RegisterMember("coverTop", &CardData::coverTop);
            card.RegisterMember("outcome", &CardData::outcome);
            card.RegisterMember("outcomeKnown", &CardData::outcomeKnown);
        }
        constructor.Bind("card", &data);
        constructor.BindEventCallback(Rml::String(game_card::kPressEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (!args.empty()) {
                                              clicked = cardButtonFromName(args.front().Get<Rml::String>());
                                          }
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(game_card::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the game card's document could not be loaded (see the log)",
                             std::string(game_card::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const GameCardContent& content) {
        data.title = Rml::String(content.title);
        data.platform = Rml::String(content.platform);
        data.year = content.year > 0 ? Rml::String(std::to_string(content.year)) : Rml::String();
        data.developer = Rml::String(content.developer);
        data.genre = Rml::String(content.genre);
        data.players = Rml::String(content.players);
        data.state = Rml::String(content.state);
        data.readiness = Rml::String(content.readiness);
        data.blocker = Rml::String(content.blocker);
        data.verified = content.verified;
        data.details = content.details;
        data.unavailable = content.unavailable;
        data.otherVersions = static_cast<int>(std::min<std::uint32_t>(content.otherVersions, 1000000));
        data.cover = content.cover;
        // The cover at its own shape in the frame (a jewel case's square cover pillarboxed), never stretched; the
        // frame's shape fills it as I3's card did.
        const CardCoverBox box = cardCoverBox(content.coverWidth, content.coverHeight);
        data.coverWidth = percent(box.width);
        data.coverHeight = percent(box.height);
        data.coverLeft = percent(box.left);
        data.coverTop = percent(box.top * static_cast<double>(game_card::kCoverFrameHeight) /
                                static_cast<double>(game_card::kCoverFrameWidth));
        data.outcome = Rml::String(content.outcome);
        data.outcomeKnown = std::find(game_card::kOutcomeCodes.begin(), game_card::kOutcomeCodes.end(),
                                      content.outcome) != game_card::kOutcomeCodes.end();
        handle.DirtyAllVariables();
    }

    /// The focused button as the model has it, unless the context's focus is there already.
    void applyFocus() {
        Rml::Element* button = document->GetElementById(Rml::String(elementIdOf(focused)));
        if (button != nullptr && view.context().GetFocusElement() != button) {
            button->Focus();
        }
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    GameCardContent content;
    CardButton focused = CardButton::Play;
    CardData data;
    std::optional<CardButton> clicked;
};

GameCardScreen::GameCardScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

GameCardScreen::~GameCardScreen() = default;

Expected<std::unique_ptr<GameCardScreen>> GameCardScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<GameCardScreen>(new GameCardScreen(std::move(impl)));
}

void GameCardScreen::show(const GameCardContent& content, CardButton focused) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    if (!impl.shown || !(impl.content == content)) {
        impl.content = content;
        impl.bind(content);
    }
    impl.focused = focused;
    if (!impl.shown) {
        // Shown without taking the focus from the world: the focus goes to the model's button just below.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.shown = true;
    }
    impl.applyFocus();
}

void GameCardScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    if (Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document) {
        focus->Blur();
    }
    impl.document->Hide();
}

bool GameCardScreen::visible() const noexcept {
    return m_impl->shown;
}

bool GameCardScreen::handlePointer(const SDL_Event& event) {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr || !isPointer(event.type)) {
        return false;
    }
    static_cast<void>(impl.view.handleEvent(event));
    if (!isPointerButton(event.type)) {
        return false;
    }
    // Over the card (its own document under the pointer, not another one's body that covers the view): the room
    // must not see the press.
    const Rml::Element* hover = impl.view.context().GetHoverElement();
    return hover != nullptr && hover->GetOwnerDocument() == impl.document;
}

std::optional<CardButton> GameCardScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

bool GameCardScreen::squeezed() const {
    const Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr) {
        return false;
    }
    return impl.document->GetScrollHeight() > impl.document->GetClientHeight() + 1.0f;
}

std::optional<glm::vec4> GameCardScreen::coverBox() const {
    const Impl& impl = *m_impl;
    if (!impl.shown || !impl.content.cover) {
        return std::nullopt;
    }
    return impl.view.elementBox(game_card::kCoverId);
}

} // namespace rv::ui
