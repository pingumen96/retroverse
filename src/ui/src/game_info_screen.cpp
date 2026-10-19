#include <rv/ui/game_info_screen.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

/// The data model's "info" (game_info.rml).
struct InfoData {
    Rml::String title;
    Rml::String platform;
    /// "" when unknown.
    Rml::String year;
    Rml::String developer;
    Rml::String publisher;
    Rml::String genre;
    Rml::String players;
    Rml::String description;
    bool details = false;
    int versionCount = 0;
    /// The versions beyond those listed.
    int more = 0;
};

/// One of the data model's "versions".
struct VersionData {
    Rml::String title;
    Rml::String region;
};

/// A line of the body's scrolling, in dp.
constexpr float kLineDp = 40.0f;

} // namespace

struct GameInfoScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(game_info::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(game_info::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(game_info::kDataModel));
        }
        modelCreated = true;
        if (auto info = constructor.RegisterStruct<InfoData>()) {
            info.RegisterMember("title", &InfoData::title);
            info.RegisterMember("platform", &InfoData::platform);
            info.RegisterMember("year", &InfoData::year);
            info.RegisterMember("developer", &InfoData::developer);
            info.RegisterMember("publisher", &InfoData::publisher);
            info.RegisterMember("genre", &InfoData::genre);
            info.RegisterMember("players", &InfoData::players);
            info.RegisterMember("description", &InfoData::description);
            info.RegisterMember("details", &InfoData::details);
            info.RegisterMember("versionCount", &InfoData::versionCount);
            info.RegisterMember("more", &InfoData::more);
        }
        if (auto version = constructor.RegisterStruct<VersionData>()) {
            version.RegisterMember("title", &VersionData::title);
            version.RegisterMember("region", &VersionData::region);
        }
        constructor.RegisterArray<std::vector<VersionData>>();
        constructor.Bind("info", &data);
        constructor.Bind("versions", &versions);
        constructor.BindEventCallback(Rml::String(game_info::kActEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          const Rml::String name = args.front().Get<Rml::String>();
                                          if (name == "close") {
                                              clicked = InfoStep::Close;
                                          } else if (name == "media") {
                                              clicked = InfoStep::Media;
                                          }
                                      });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(game_info::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the information screen's document could not be loaded (see the log)",
                             std::string(game_info::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const GameInfoContent& next) {
        data.title = Rml::String(next.title);
        data.platform = Rml::String(next.platform);
        data.year = next.year > 0 ? Rml::String(std::to_string(next.year)) : Rml::String();
        data.developer = Rml::String(next.developer);
        data.publisher = Rml::String(next.publisher);
        data.genre = Rml::String(next.genre);
        data.players = Rml::String(next.players);
        data.description = Rml::String(next.description);
        data.details = next.details;
        const std::size_t listed = std::min(next.versions.size(), game_info::kMaxVersions);
        const std::uint32_t total = std::max<std::uint32_t>(next.versionCount, static_cast<std::uint32_t>(listed));
        data.versionCount = static_cast<int>(std::min<std::uint32_t>(total, 1000000));
        data.more = static_cast<int>(std::min<std::uint32_t>(total - static_cast<std::uint32_t>(listed), 1000000));
        versions.clear();
        for (std::size_t index = 0; index < listed; ++index) {
            versions.push_back(VersionData{Rml::String(next.versions[index].title),
                                           Rml::String(next.versions[index].region)});
        }
        handle.DirtyAllVariables();
    }

    [[nodiscard]] Rml::Element* body() const {
        return document != nullptr ? document->GetElementById(Rml::String(game_info::kBodyId)) : nullptr;
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    std::optional<GameInfoContent> content;
    InfoData data;
    std::vector<VersionData> versions;
    std::optional<InfoStep> clicked;
};

GameInfoScreen::GameInfoScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

GameInfoScreen::~GameInfoScreen() = default;

Expected<std::unique_ptr<GameInfoScreen>> GameInfoScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<GameInfoScreen>(new GameInfoScreen(std::move(impl)));
}

void GameInfoScreen::show(const GameInfoContent& content) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    const bool otherGame = !impl.content || impl.content->gameId != content.gameId;
    if (!impl.content || !(*impl.content == content)) {
        impl.content = content;
        impl.bind(content);
    }
    if (!impl.shown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.shown = true;
    }
    if (otherGame) {
        if (Rml::Element* body = impl.body()) {
            body->SetScrollTop(0.0f);
        }
    }
}

void GameInfoScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
}

bool GameInfoScreen::visible() const noexcept {
    return m_impl->shown;
}

bool GameInfoScreen::scroll(InfoStep step) {
    Impl& impl = *m_impl;
    Rml::Element* body = impl.shown ? impl.body() : nullptr;
    if (body == nullptr) {
        return false;
    }
    const float top = body->GetScrollTop();
    const float next = scrolledTop(top, step, body->GetClientHeight(), body->GetScrollHeight(),
                                   kLineDp * impl.view.dpRatio());
    if (next == top) {
        return false;
    }
    body->SetScrollTop(next);
    return true;
}

float GameInfoScreen::scrollTop() const {
    Rml::Element* body = m_impl->shown ? m_impl->body() : nullptr;
    return body != nullptr ? body->GetScrollTop() : 0.0f;
}

float GameInfoScreen::scrollRange() const {
    Rml::Element* body = m_impl->shown ? m_impl->body() : nullptr;
    return body != nullptr ? std::max(0.0f, body->GetScrollHeight() - body->GetClientHeight()) : 0.0f;
}

std::optional<InfoStep> GameInfoScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

} // namespace rv::ui
