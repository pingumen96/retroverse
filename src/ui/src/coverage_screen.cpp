#include <rv/ui/coverage_screen.hpp>

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

/// A line of the details' scrolling, in dp.
constexpr float kLineDp = 44.0f;

/// A system's row as the document reads it.
struct RowView {
    int index = 0;
    Rml::String elementId;
    Rml::String name;
    Rml::String fullName;
    int games = 0;
    Rml::String covers;
    Rml::String boxes;
    Rml::String screenshots;
    Rml::String marquees;
    Rml::String videos;
    Rml::String any;
    int clipFailures = 0;
};

/// A kind of the system's details, in both populations.
struct KindView {
    Rml::String kind;
    Rml::String gamesShare;
    int gamesWith = 0;
    int gamesMissing = 0;
    bool gamesLow = false;
    Rml::String scrapedShare;
    int scrapedWith = 0;
    int scrapedMissing = 0;
    bool scrapedLow = false;
};

struct ClipView {
    Rml::String file;
    Rml::String verdict;
};

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

[[nodiscard]] int asInt(std::uint64_t value) noexcept {
    return static_cast<int>(std::min<std::uint64_t>(value, 2'000'000'000));
}

[[nodiscard]] RowView rowView(const CoverageRow& row, int index) {
    return RowView{index,
                   Rml::String(CoverageModel::rowElement(static_cast<std::size_t>(std::max(index, 0)))),
                   Rml::String(row.name),
                   Rml::String(row.fullName),
                   asInt(row.games),
                   Rml::String(row.covers),
                   Rml::String(row.boxes),
                   Rml::String(row.screenshots),
                   Rml::String(row.marquees),
                   Rml::String(row.videos),
                   Rml::String(row.any),
                   asInt(row.clipFailures)};
}

/// The element `id` of `panel`'s document, shown, runs past the panel's bottom: what no overflow check of a text sees.
[[nodiscard]] std::uint32_t pastPanel(Rml::ElementDocument& document, Rml::Element& panel, const char* id) {
    Rml::Element* element = document.GetElementById(id);
    if (element == nullptr || !element->IsVisible()) {
        return 0;
    }
    const auto bottom = [](Rml::Element& shown) {
        return shown.GetAbsoluteOffset(Rml::BoxArea::Border).y + shown.GetBox().GetSize(Rml::BoxArea::Border).y;
    };
    return bottom(*element) > bottom(panel) + 1.0f ? 1 : 0;
}

} // namespace

struct CoverageScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(coverage::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(coverage::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(coverage::kDataModel));
        }
        modelCreated = true;
        if (auto row = constructor.RegisterStruct<RowView>()) {
            row.RegisterMember("index", &RowView::index);
            row.RegisterMember("elementId", &RowView::elementId);
            row.RegisterMember("name", &RowView::name);
            row.RegisterMember("fullName", &RowView::fullName);
            row.RegisterMember("games", &RowView::games);
            row.RegisterMember("covers", &RowView::covers);
            row.RegisterMember("boxes", &RowView::boxes);
            row.RegisterMember("screenshots", &RowView::screenshots);
            row.RegisterMember("marquees", &RowView::marquees);
            row.RegisterMember("videos", &RowView::videos);
            row.RegisterMember("any", &RowView::any);
            row.RegisterMember("clipFailures", &RowView::clipFailures);
        }
        constructor.RegisterArray<std::vector<RowView>>();
        if (auto kind = constructor.RegisterStruct<KindView>()) {
            kind.RegisterMember("kind", &KindView::kind);
            kind.RegisterMember("gamesShare", &KindView::gamesShare);
            kind.RegisterMember("gamesWith", &KindView::gamesWith);
            kind.RegisterMember("gamesMissing", &KindView::gamesMissing);
            kind.RegisterMember("gamesLow", &KindView::gamesLow);
            kind.RegisterMember("scrapedShare", &KindView::scrapedShare);
            kind.RegisterMember("scrapedWith", &KindView::scrapedWith);
            kind.RegisterMember("scrapedMissing", &KindView::scrapedMissing);
            kind.RegisterMember("scrapedLow", &KindView::scrapedLow);
        }
        constructor.RegisterArray<std::vector<KindView>>();
        if (auto clip = constructor.RegisterStruct<ClipView>()) {
            clip.RegisterMember("file", &ClipView::file);
            clip.RegisterMember("verdict", &ClipView::verdict);
        }
        constructor.RegisterArray<std::vector<ClipView>>();
        constructor.Bind("state", &state);
        constructor.Bind("viewName", &viewName);
        constructor.Bind("tab", &tab);
        constructor.Bind("total", &total);
        constructor.Bind("rows", &rows);
        constructor.Bind("current", &current);
        constructor.Bind("currentScraped", &currentScraped);
        constructor.Bind("kinds", &kinds);
        constructor.Bind("clips", &clips);
        constructor.Bind("clipCount", &clipCount);
        constructor.Bind("clipsUnknown", &clipsUnknown);
        constructor.BindEventCallback("section", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) {
                return;
            }
            if (const auto chosen = coverageTabFromName(args.front().Get<Rml::String>())) {
                clicked = CoverageClick{CoverageClick::Kind::Tab, *chosen, 0};
            }
        });
        constructor.BindEventCallback("system", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            clicked = CoverageClick{CoverageClick::Kind::System, CoverageTab::Games,
                                    args.empty() ? -1 : args.front().Get<int>(-1)};
        });
        constructor.BindEventCallback("close", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            clicked = CoverageClick{CoverageClick::Kind::Close, CoverageTab::Games, 0};
        });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(coverage::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the coverage document could not be loaded (see the log)",
                             std::string(coverage::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    void bind(const CoverageModel& shown) {
        const Rml::String nextView(toString(shown.view()));
        const bool otherView = nextView != viewName;
        state = Rml::String(toString(shown.state()));
        viewName = nextView;
        tab = Rml::String(toString(shown.tab()));
        const CoverageContent& content = shown.content();
        total = rowView(content.table(shown.tab()).total, -1);
        rows.clear();
        int index = 0;
        for (const CoverageRow& row : shown.rows()) {
            rows.push_back(rowView(row, index++));
        }
        current = RowView{};
        currentScraped = RowView{};
        kinds.clear();
        clips.clear();
        if (const CoverageRow* row = shown.system()) {
            current = rowView(*row, static_cast<int>(shown.focused()));
            const auto find = [&](const CoverageTable& table) -> const CoverageRow* {
                const auto found = std::find_if(table.systems.begin(), table.systems.end(),
                                                [&](const CoverageRow& other) { return other.name == row->name; });
                return found != table.systems.end() ? &*found : nullptr;
            };
            const CoverageRow* games = find(content.games);
            const CoverageRow* scraped = find(content.scraped);
            if (scraped != nullptr) {
                currentScraped = rowView(*scraped, -1);
            }
            const std::size_t count = games != nullptr ? games->kinds.size() : 0;
            for (std::size_t kind = 0; kind < count; ++kind) {
                KindView made;
                const CoverageShare& all = games->kinds[kind];
                made.kind = Rml::String(all.kind);
                made.gamesShare = Rml::String(all.share);
                made.gamesWith = asInt(all.with);
                made.gamesMissing = asInt(all.missing);
                made.gamesLow = all.low;
                if (scraped != nullptr && kind < scraped->kinds.size()) {
                    const CoverageShare& listed = scraped->kinds[kind];
                    made.scrapedShare = Rml::String(listed.share);
                    made.scrapedWith = asInt(listed.with);
                    made.scrapedMissing = asInt(listed.missing);
                    made.scrapedLow = listed.low;
                } else {
                    made.scrapedShare = "-";
                }
                kinds.push_back(std::move(made));
            }
            for (const CoverageClip& clip : shown.systemClips()) {
                clips.push_back(ClipView{Rml::String(clip.file), Rml::String(clip.verdict)});
            }
        }
        clipCount = asInt(content.clips.size());
        clipsUnknown = content.clipsUnknown;
        handle.DirtyAllVariables();
        if (otherView) {
            if (Rml::Element* detail = document->GetElementById(Rml::String(coverage::kDetailId))) {
                detail->SetScrollTop(0.0f);
            }
        }
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
        if (element->Focus(true)) {
            element->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        }
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool shown = false;
    const CoverageModel* model = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<CoverageClick> clicked;

    Rml::String state;
    Rml::String viewName;
    Rml::String tab;
    RowView total;
    std::vector<RowView> rows;
    RowView current;
    RowView currentScraped;
    std::vector<KindView> kinds;
    std::vector<ClipView> clips;
    int clipCount = 0;
    bool clipsUnknown = false;
};

CoverageScreen::CoverageScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

CoverageScreen::~CoverageScreen() = default;

Expected<std::unique_ptr<CoverageScreen>> CoverageScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<CoverageScreen>(new CoverageScreen(std::move(impl)));
}

void CoverageScreen::show(const CoverageModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.model = &model;
    if (impl.boundRevision != model.revision()) {
        impl.bind(model);
        impl.boundRevision = model.revision();
    }
    if (!impl.shown) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.shown = true;
    }
}

void CoverageScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.model = nullptr;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
    // As the directory's: the focus a click gave never stays on an element of the hidden document.
    if (const Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document && focus != impl.document) {
        static_cast<void>(impl.document->Focus());
    }
}

bool CoverageScreen::visible() const noexcept {
    return m_impl->shown;
}

void CoverageScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (impl.shown && impl.document != nullptr) {
        impl.applyFocus();
    }
}

bool CoverageScreen::scroll(InfoStep step) {
    Impl& impl = *m_impl;
    Rml::Element* detail =
        impl.shown && impl.document != nullptr ? impl.document->GetElementById(Rml::String(coverage::kDetailId)) : nullptr;
    if (detail == nullptr || !detail->IsVisible()) {
        return false;
    }
    const float top = detail->GetScrollTop();
    const float next = scrolledTop(top, step, detail->GetClientHeight(), detail->GetScrollHeight(),
                                   kLineDp * impl.view.dpRatio());
    if (next == top) {
        return false;
    }
    detail->SetScrollTop(next);
    return true;
}

bool CoverageScreen::handlePointer(const SDL_Event& event) {
    Impl& impl = *m_impl;
    if (!impl.shown || impl.document == nullptr || !isPointer(event.type)) {
        return false;
    }
    static_cast<void>(impl.view.handleEvent(event));
    if (!isPointerButton(event.type)) {
        return false;
    }
    const Rml::Element* hover = impl.view.context().GetHoverElement();
    return hover != nullptr && hover->GetOwnerDocument() == impl.document && hover != impl.document;
}

std::optional<CoverageClick> CoverageScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::uint32_t CoverageScreen::textCut() const {
    Rml::ElementDocument* document = m_impl->document;
    Rml::Element* panel = document != nullptr ? document->GetElementById(Rml::String(coverage::kPanelId)) : nullptr;
    if (panel == nullptr || !m_impl->shown) {
        return 0;
    }
    return detail::elementsWithCutText(*panel) + pastPanel(*document, *panel, "cv-list") +
           pastPanel(*document, *panel, "cv-detail") + pastPanel(*document, *panel, "cv-footer");
}

std::string CoverageScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

} // namespace rv::ui
