#include <rv/ui/library_screen.hpp>

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

/// A line of the body's scrolling, in dp.
constexpr float kLineDp = 44.0f;

struct RootView {
    Rml::String kind;
    Rml::String system;
    Rml::String path;
    Rml::String state;
    Rml::String mode;
};

struct IssueView {
    Rml::String code;
    Rml::String severity;
    int count = 0;
    Rml::String systems;
    int moreSystems = 0;
};

struct HeldView {
    Rml::String name;
    Rml::String fullName;
    int gone = 0;
    int present = 0;
};

struct ReviewView {
    int index = 0;
    Rml::String elementId;
    Rml::String system;
    Rml::String title;
    Rml::String vanished;
    int candidates = 0;
};

struct CandidateView {
    int index = 0;
    Rml::String elementId;
    Rml::String path;
    bool offered = false;
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

struct LibraryScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(library::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(library::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(library::kDataModel));
        }
        modelCreated = true;
        if (auto row = constructor.RegisterStruct<RootView>()) {
            row.RegisterMember("kind", &RootView::kind);
            row.RegisterMember("system", &RootView::system);
            row.RegisterMember("path", &RootView::path);
            row.RegisterMember("state", &RootView::state);
            row.RegisterMember("mode", &RootView::mode);
        }
        constructor.RegisterArray<std::vector<RootView>>();
        if (auto row = constructor.RegisterStruct<IssueView>()) {
            row.RegisterMember("code", &IssueView::code);
            row.RegisterMember("severity", &IssueView::severity);
            row.RegisterMember("count", &IssueView::count);
            row.RegisterMember("systems", &IssueView::systems);
            row.RegisterMember("moreSystems", &IssueView::moreSystems);
        }
        constructor.RegisterArray<std::vector<IssueView>>();
        if (auto row = constructor.RegisterStruct<HeldView>()) {
            row.RegisterMember("name", &HeldView::name);
            row.RegisterMember("fullName", &HeldView::fullName);
            row.RegisterMember("gone", &HeldView::gone);
            row.RegisterMember("present", &HeldView::present);
        }
        constructor.RegisterArray<std::vector<HeldView>>();
        if (auto row = constructor.RegisterStruct<ReviewView>()) {
            row.RegisterMember("index", &ReviewView::index);
            row.RegisterMember("elementId", &ReviewView::elementId);
            row.RegisterMember("system", &ReviewView::system);
            row.RegisterMember("title", &ReviewView::title);
            row.RegisterMember("vanished", &ReviewView::vanished);
            row.RegisterMember("candidates", &ReviewView::candidates);
        }
        constructor.RegisterArray<std::vector<ReviewView>>();
        if (auto row = constructor.RegisterStruct<CandidateView>()) {
            row.RegisterMember("index", &CandidateView::index);
            row.RegisterMember("elementId", &CandidateView::elementId);
            row.RegisterMember("path", &CandidateView::path);
            row.RegisterMember("offered", &CandidateView::offered);
        }
        constructor.RegisterArray<std::vector<CandidateView>>();
        constructor.Bind("state", &state);
        constructor.Bind("viewName", &viewName);
        constructor.Bind("tab", &tab);
        constructor.Bind("roots", &roots);
        constructor.Bind("unavailableSystems", &unavailableSystems);
        constructor.Bind("importKnown", &importKnown);
        constructor.Bind("importWhen", &importWhen);
        constructor.Bind("importKind", &importKind);
        constructor.Bind("importSystems", &importSystems);
        constructor.Bind("importAccepted", &importAccepted);
        constructor.Bind("canRefresh", &canRefresh);
        constructor.Bind("refreshing", &refreshing);
        constructor.Bind("phase", &phase);
        constructor.Bind("refreshDone", &refreshDone);
        constructor.Bind("refreshTotal", &refreshTotal);
        constructor.Bind("lastRefresh", &lastRefresh);
        constructor.Bind("added", &added);
        constructor.Bind("removed", &removed);
        constructor.Bind("guarded", &guarded);
        constructor.Bind("watching", &watching);
        constructor.Bind("issues", &issues);
        constructor.Bind("issueCount", &issueCount);
        constructor.Bind("held", &held);
        constructor.Bind("reviews", &reviews);
        constructor.Bind("current", &current);
        constructor.Bind("candidates", &candidates);
        constructor.Bind("chosen", &chosen);
        constructor.Bind("resolving", &resolving);
        constructor.Bind("problem", &problem);
        constructor.Bind("files", &files);
        constructor.Bind("hashed", &hashed);
        constructor.Bind("hash", &hash);
        constructor.Bind("hashDone", &hashDone);
        constructor.Bind("hashTotal", &hashTotal);
        constructor.Bind("hashHashed", &hashHashed);
        constructor.Bind("hashUnreadable", &hashUnreadable);
        constructor.Bind("hashRemaining", &hashRemaining);
        constructor.BindEventCallback("section", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) {
                return;
            }
            if (const auto chosenTab = libraryTabFromName(args.front().Get<Rml::String>())) {
                LibraryClick made;
                made.kind = LibraryClick::Kind::Tab;
                made.tab = *chosenTab;
                clicked = made;
            }
        });
        constructor.BindEventCallback("button", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) {
                return;
            }
            if (const auto button = libraryButtonFromName(args.front().Get<Rml::String>())) {
                LibraryClick made;
                made.kind = LibraryClick::Kind::Button;
                made.button = *button;
                clicked = made;
            }
        });
        constructor.BindEventCallback("review", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            LibraryClick made;
            made.kind = LibraryClick::Kind::Review;
            made.index = args.empty() ? -1 : args.front().Get<int>(-1);
            clicked = made;
        });
        constructor.BindEventCallback("candidate", [this](Rml::DataModelHandle, Rml::Event&,
                                                           const Rml::VariantList& args) {
            LibraryClick made;
            made.kind = LibraryClick::Kind::Candidate;
            made.index = args.empty() ? -1 : args.front().Get<int>(-1);
            clicked = made;
        });
        constructor.BindEventCallback("close", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            LibraryClick made;
            made.kind = LibraryClick::Kind::Close;
            clicked = made;
        });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(library::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the library document could not be loaded (see the log)",
                             std::string(library::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    [[nodiscard]] Rml::Element* body() const {
        return document != nullptr ? document->GetElementById(Rml::String(library::kBodyId)) : nullptr;
    }

    void bind(const LibraryModel& shown) {
        const Rml::String nextTab(toString(shown.tab()));
        const Rml::String nextView(toString(shown.view()));
        const bool moved = nextTab != tab || nextView != viewName;
        state = Rml::String(toString(shown.state()));
        viewName = nextView;
        tab = nextTab;
        const LibraryContent& content = shown.content();
        const LibraryLive& live = shown.live();
        roots.clear();
        for (const LibraryRoot& root : withModes(content.roots, live)) {
            roots.push_back(RootView{Rml::String(root.kind), Rml::String(root.system), Rml::String(root.path),
                                     Rml::String(root.state), Rml::String(root.mode)});
        }
        unavailableSystems = asInt(content.unavailableSystems);
        importKnown = content.lastImport.known;
        importWhen = Rml::String(content.lastImport.when);
        importKind = Rml::String(content.lastImport.kind);
        importSystems = asInt(content.lastImport.systems);
        importAccepted = content.lastImport.acceptRemovals;
        canRefresh = live.canRefresh;
        refreshing = live.refreshing;
        phase = Rml::String(live.phase);
        refreshDone = asInt(live.refreshDone);
        refreshTotal = asInt(live.refreshTotal);
        lastRefresh = Rml::String(live.lastRefresh);
        added = asInt(live.added);
        removed = asInt(live.removed);
        guarded = asInt(live.guarded);
        watching = live.watching;
        issues.clear();
        for (const LibraryIssueGroup& group : content.issues) {
            issues.push_back(IssueView{Rml::String(group.code), Rml::String(group.severity), asInt(group.count),
                                       Rml::String(group.systems), asInt(group.moreSystems)});
        }
        issueCount = asInt(content.issueCount);
        held.clear();
        for (const LibraryHeld& system : content.held) {
            held.push_back(HeldView{Rml::String(system.name), Rml::String(system.fullName), asInt(system.gone),
                                    asInt(system.present)});
        }
        reviews.clear();
        int index = 0;
        for (const LibraryReview& review : content.reviews) {
            reviews.push_back(ReviewView{index, Rml::String(LibraryModel::reviewElement(static_cast<std::size_t>(index))),
                                         Rml::String(review.system), Rml::String(review.title),
                                         Rml::String(review.vanished), asInt(review.candidates.size())});
            ++index;
        }
        current = ReviewView{};
        candidates.clear();
        if (const LibraryReview* review = shown.review()) {
            current.system = Rml::String(review->system);
            current.title = Rml::String(review->title);
            current.vanished = Rml::String(review->vanished);
            current.candidates = asInt(review->candidates.size());
            index = 0;
            for (const LibraryCandidate& candidate : review->candidates) {
                candidates.push_back(CandidateView{
                    index, Rml::String(LibraryModel::candidateElement(static_cast<std::size_t>(index))),
                    Rml::String(candidate.path), !candidate.gameId.empty()});
                ++index;
            }
        }
        const LibraryCandidate* candidate = shown.candidate();
        chosen = candidate != nullptr ? Rml::String(candidate->path) : Rml::String();
        resolving = shown.resolving();
        problem = Rml::String(shown.problem());
        files = asInt(content.files);
        hashed = asInt(content.hashed);
        hash = Rml::String(live.hash);
        hashDone = asInt(live.hashDone);
        hashTotal = asInt(live.hashTotal);
        hashHashed = asInt(live.hashHashed);
        hashUnreadable = asInt(live.hashUnreadable);
        hashRemaining = asInt(live.hashRemaining);
        handle.DirtyAllVariables();
        if (moved) {
            if (Rml::Element* scrolled = body()) {
                scrolled->SetScrollTop(0.0f);
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
    const LibraryModel* model = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::optional<LibraryClick> clicked;

    Rml::String state;
    Rml::String viewName;
    Rml::String tab;
    std::vector<RootView> roots;
    int unavailableSystems = 0;
    bool importKnown = false;
    Rml::String importWhen;
    Rml::String importKind;
    int importSystems = 0;
    bool importAccepted = false;
    bool canRefresh = false;
    bool refreshing = false;
    Rml::String phase;
    int refreshDone = 0;
    int refreshTotal = 0;
    Rml::String lastRefresh;
    int added = 0;
    int removed = 0;
    int guarded = 0;
    bool watching = false;
    std::vector<IssueView> issues;
    int issueCount = 0;
    std::vector<HeldView> held;
    std::vector<ReviewView> reviews;
    ReviewView current;
    std::vector<CandidateView> candidates;
    Rml::String chosen;
    bool resolving = false;
    Rml::String problem;
    int files = 0;
    int hashed = 0;
    Rml::String hash;
    int hashDone = 0;
    int hashTotal = 0;
    int hashHashed = 0;
    int hashUnreadable = 0;
    int hashRemaining = 0;
};

LibraryScreen::LibraryScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

LibraryScreen::~LibraryScreen() = default;

Expected<std::unique_ptr<LibraryScreen>> LibraryScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<LibraryScreen>(new LibraryScreen(std::move(impl)));
}

void LibraryScreen::show(const LibraryModel& model) {
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

void LibraryScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.model = nullptr;
    if (!impl.shown || impl.document == nullptr) {
        impl.shown = false;
        return;
    }
    impl.shown = false;
    impl.document->Hide();
    if (const Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document && focus != impl.document) {
        static_cast<void>(impl.document->Focus());
    }
}

bool LibraryScreen::visible() const noexcept {
    return m_impl->shown;
}

void LibraryScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (impl.shown && impl.document != nullptr) {
        impl.applyFocus();
    }
}

bool LibraryScreen::scroll(InfoStep step) {
    Impl& impl = *m_impl;
    Rml::Element* body = impl.shown ? impl.body() : nullptr;
    if (body == nullptr) {
        return false;
    }
    const float top = body->GetScrollTop();
    const float next =
        scrolledTop(top, step, body->GetClientHeight(), body->GetScrollHeight(), kLineDp * impl.view.dpRatio());
    if (next == top) {
        return false;
    }
    body->SetScrollTop(next);
    return true;
}

bool LibraryScreen::handlePointer(const SDL_Event& event) {
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

std::optional<LibraryClick> LibraryScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::uint32_t LibraryScreen::textCut() const {
    Rml::ElementDocument* document = m_impl->document;
    Rml::Element* panel = document != nullptr ? document->GetElementById(Rml::String(library::kPanelId)) : nullptr;
    if (panel == nullptr || !m_impl->shown) {
        return 0;
    }
    return detail::elementsWithCutText(*panel) + pastPanel(*document, *panel, "lb-body") +
           pastPanel(*document, *panel, "lb-footer");
}

std::string LibraryScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

} // namespace rv::ui
