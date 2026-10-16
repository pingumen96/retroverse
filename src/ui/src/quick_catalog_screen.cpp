#include <rv/ui/quick_catalog_screen.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ScrollTypes.h>
#include <RmlUi/Core/Variant.h>
#include <glm/vec3.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/file_watch_set.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/focus_regions.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

namespace qc = quick_catalog;

/// A filter chip as the document binds it.
struct ChipRow {
    Rml::String elementId;
    Rml::String key;
    Rml::String label;
    bool active = false;
};

/// A visible game as the document binds it.
struct GameRow {
    Rml::String elementId;
    Rml::String title;
    Rml::String platform;
    Rml::String year;
    bool selected = false;
};

/// The selected game's card.
struct CardData {
    bool present = false;
    bool expanded = false;
    Rml::String title;
    Rml::String platform;
    Rml::String year;
    Rml::String developer;
    Rml::String publisher;
    Rml::String genre;
    Rml::String players;
    Rml::String description;
};

/// "n.d." (non disponibile) for an unknown year or field.
[[nodiscard]] Rml::String orUnknown(const std::string& text) {
    return text.empty() ? Rml::String("n.d.") : Rml::String(text);
}

[[nodiscard]] Rml::String yearText(int year) {
    return year > 0 ? std::to_string(year) : Rml::String("n.d.");
}

[[nodiscard]] Rml::String gameElementId(std::string_view entryId) {
    return Rml::String(qc::kGamePrefix) + Rml::String(entryId);
}

[[nodiscard]] Rml::String chipElementId(std::size_t index) {
    return Rml::String(qc::kChipPrefix) + std::to_string(index);
}

/// The region of `element`: the nav-region attribute of the element or of its closest ancestor that has one.
[[nodiscard]] Rml::String regionOf(Rml::Element* element) {
    const Rml::String attribute(qc::kRegionAttribute);
    for (Rml::Element* current = element; current != nullptr; current = current->GetParentNode()) {
        if (const Rml::Variant* region = current->GetAttribute(attribute)) {
            return region->Get<Rml::String>();
        }
    }
    return {};
}

/// Seconds the selection must rest before the card follows it, when it moved less than this long before (a held
/// direction runs through the list at 10 to 20 games per second): a single move updates the card at once.
constexpr double kCardSettleSeconds = 0.15;

/// Events the screen listens to on its document: every focus change (capture phase: focus does not bubble), double
/// clicks on games, the line break that ends a search.
constexpr std::array<Rml::EventId, 3> kListenedEvents{Rml::EventId::Focus, Rml::EventId::Dblclick,
                                                      Rml::EventId::Change};

} // namespace

struct QuickCatalogScreen::Impl {
    /// Forwards the document's events to the screen.
    class Listener final : public Rml::EventListener {
    public:
        explicit Listener(Impl& impl) noexcept : m_impl(impl) {}
        void ProcessEvent(Rml::Event& event) override { m_impl.onEvent(event); }

    private:
        Impl& m_impl;
    };

    Impl(UiView& view, QuickCatalogModel& model, QuickCatalogEvents events)
        : view(view), model(model), events(std::move(events)), listener(*this) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        unloadDocument();
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(qc::kDataModel));
        }
    }

    // --- Data model ------------------------------------------------------------------------------------------------

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(qc::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(qc::kDataModel));
        }
        modelCreated = true;
        if (auto chip = constructor.RegisterStruct<ChipRow>()) {
            chip.RegisterMember("elementId", &ChipRow::elementId);
            chip.RegisterMember("key", &ChipRow::key);
            chip.RegisterMember("label", &ChipRow::label);
            chip.RegisterMember("active", &ChipRow::active);
        }
        constructor.RegisterArray<std::vector<ChipRow>>();
        if (auto game = constructor.RegisterStruct<GameRow>()) {
            game.RegisterMember("elementId", &GameRow::elementId);
            game.RegisterMember("title", &GameRow::title);
            game.RegisterMember("platform", &GameRow::platform);
            game.RegisterMember("year", &GameRow::year);
            game.RegisterMember("selected", &GameRow::selected);
        }
        constructor.RegisterArray<std::vector<GameRow>>();
        if (auto cardStruct = constructor.RegisterStruct<CardData>()) {
            cardStruct.RegisterMember("present", &CardData::present);
            cardStruct.RegisterMember("expanded", &CardData::expanded);
            cardStruct.RegisterMember("title", &CardData::title);
            cardStruct.RegisterMember("platform", &CardData::platform);
            cardStruct.RegisterMember("year", &CardData::year);
            cardStruct.RegisterMember("developer", &CardData::developer);
            cardStruct.RegisterMember("publisher", &CardData::publisher);
            cardStruct.RegisterMember("genre", &CardData::genre);
            cardStruct.RegisterMember("players", &CardData::players);
            cardStruct.RegisterMember("description", &CardData::description);
        }
        constructor.Bind("chips", &chips);
        constructor.Bind("games", &games);
        constructor.Bind("card", &card);
        constructor.Bind("status", &status);
        constructor.Bind("count", &count);
        constructor.BindFunc(
            "search", [this](Rml::Variant& value) { value = Rml::String(model.search()); },
            [this](const Rml::Variant& value) { setSearch(value.Get<Rml::String>()); });
        constructor.BindEventCallback("filter",
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (!args.empty()) {
                                              applyFilter(args.front().Get<Rml::String>());
                                          }
                                      });
        constructor.BindEventCallback("launch",
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { launch(); });
        constructor.BindEventCallback("media",
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { media(); });
        constructor.BindEventCallback("info",
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { info(); });
        handle = constructor.GetModelHandle();
        return ok();
    }

    /// The chips when the active filter changed; the visible games and their count when they changed. The two are
    /// independent: a filter that lets the same games through (a search that left one game, whose platform is chosen)
    /// still moves the highlight to its chip.
    void syncList() {
        syncChips();
        if (model.visibleRevision() == shownVisible) {
            return;
        }
        shownVisible = model.visibleRevision();
        const CatalogEntry* selected = model.selected();
        games.clear();
        for (const std::size_t index : model.visible()) {
            const CatalogEntry& entry = model.entries()[index];
            games.push_back(GameRow{gameElementId(entry.id), entry.title, entry.platform, yearText(entry.year),
                                    selected != nullptr && selected->id == entry.id});
        }
        count = static_cast<int>(games.size());
        // The game the list region remembers may be gone: the selected game is its entry then.
        if (const std::optional<std::string> remembered = regions.remembered(qc::kRegionList)) {
            if (!remembered->starts_with(qc::kGamePrefix) ||
                !model.isVisible(std::string_view(*remembered).substr(qc::kGamePrefix.size()))) {
                regions.forget(qc::kRegionList);
            }
        }
        handle.DirtyVariable("games");
        handle.DirtyVariable("count");
        syncSelection(); // when the filter or the search moved the selection
    }

    /// The chips, their active flags and the chips region's entry (the active chip), when the active filter changed.
    void syncChips() {
        if (shownFilter && *shownFilter == model.filterRevision()) {
            return;
        }
        shownFilter = model.filterRevision();
        const std::span<const PlatformFilter> filters = model.filters();
        chips.resize(filters.size());
        for (std::size_t index = 0; index < filters.size(); ++index) {
            chips[index].elementId = chipElementId(index);
            chips[index].key = filters[index].key;
            chips[index].label = filters[index].label;
            chips[index].active = filters[index].key == model.activeFilter();
            if (chips[index].active) {
                regions.setEntry(qc::kRegionChips, chips[index].elementId);
            }
        }
        handle.DirtyVariable("chips");
    }

    /// The selected flags of the rows and the card, when the selection changed.
    void syncSelection() {
        if (model.selectionRevision() == shownSelection) {
            return;
        }
        shownSelection = model.selectionRevision();
        const CatalogEntry* selected = model.selected();
        const Rml::String selectedId = selected != nullptr ? gameElementId(selected->id) : Rml::String();
        for (GameRow& row : games) {
            row.selected = row.elementId == selectedId;
        }
        regions.setEntry(qc::kRegionList, selectedId);
        // The status line speaks of the game it was set for: another game clears it.
        if (!status.empty()) {
            status.clear();
            handle.DirtyVariable("status");
        }
        handle.DirtyVariable("games");
        // The card follows at once after a pause, and once the focus rests while it runs through the list (a held
        // direction): laying the card's text out again is most of a focus move's cost (docs/spikes/i1-ui.md).
        const double now = view.system().time();
        const bool resting = !lastSelectionChange || now - *lastSelectionChange >= kCardSettleSeconds;
        lastSelectionChange = now;
        if (resting || selected == nullptr) {
            refreshCard();
        } else {
            cardPending = true;
        }
    }

    /// The card of the selected game.
    void refreshCard() {
        cardPending = false;
        ++cardRefreshes;
        const CatalogEntry* selected = model.selected();
        card.present = selected != nullptr;
        if (selected != nullptr) {
            card.title = selected->title;
            card.platform = orUnknown(selected->platform);
            card.year = yearText(selected->year);
            card.developer = orUnknown(selected->developer);
            card.publisher = orUnknown(selected->publisher);
            card.genre = orUnknown(selected->genre);
            card.players = orUnknown(selected->players);
            card.description = selected->description;
        }
        handle.DirtyVariable("card");
    }

    /// Refreshes a card left behind by quick moves once the focus has rested for kCardSettleSeconds.
    void settleCard() {
        if (cardPending && lastSelectionChange && view.system().time() - *lastSelectionChange >= kCardSettleSeconds) {
            refreshCard();
        }
    }

    /// Refreshes a card left behind by quick moves at once: before an action on the selected game, and when the focus
    /// leaves the list (the card's actions, Avvia first, act on the selected game: the card must show it).
    void settleCardNow() {
        if (cardPending) {
            refreshCard();
        }
    }

    void setSearch(const std::string& text) {
        model.setSearch(text);
        syncList();
    }

    void applyFilter(const std::string& key) {
        if (!model.setFilter(key)) {
            spdlog::warn("Quick catalog: unknown filter '{}'", key);
            return;
        }
        spdlog::info("Quick catalog: filter '{}', {} game(s)", key.empty() ? "Tutte" : key, model.visible().size());
        syncList();
    }

    // --- Actions ---------------------------------------------------------------------------------------------------

    void launch() {
        settleCardNow();
        if (const CatalogEntry* selected = model.selected(); selected != nullptr && events.launch) {
            events.launch(*selected);
        }
    }

    void media() {
        settleCardNow();
        if (const CatalogEntry* selected = model.selected(); selected != nullptr && events.media) {
            events.media(*selected);
        }
    }

    void info() {
        const CatalogEntry* selected = model.selected();
        if (selected == nullptr) {
            return;
        }
        settleCardNow();
        card.expanded = !card.expanded;
        handle.DirtyVariable("card");
        if (events.info) {
            events.info(*selected, card.expanded);
        }
    }

    // --- Document --------------------------------------------------------------------------------------------------

    Status loadDocument() {
        Rml::ElementDocument* loaded = view.context().LoadDocument(Rml::String(qc::kDocumentPath));
        if (loaded == nullptr) {
            return makeError(ErrorCode::IoError, "the quick catalog document could not be loaded (see the log)",
                             std::string(qc::kDocumentPath));
        }
        document = loaded;
        for (const Rml::EventId id : kListenedEvents) {
            document->AddEventListener(id, &listener, id == Rml::EventId::Focus);
        }
        document->SetClass(Rml::String(qc::kHighContrastClass), highContrast);
        if (visible) {
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        }
        // Generates the rows of the data views, so that the focus can land on them.
        view.context().Update();
        chipsRow = document->GetElementById(Rml::String(qc::kChipsId));
        chipsBox = document->GetElementById(Rml::String(qc::kChipsBoxId));
        chipsHiddenBefore = false; // the new document's box has neither class yet
        chipsHiddenAfter = false;
        chipsRowSeen.reset();
        updateChipsCue();
        return ok();
    }

    void unloadDocument() {
        if (document == nullptr) {
            return;
        }
        // The listener is ours: detached before the document is released at the context's next update.
        for (const Rml::EventId id : kListenedEvents) {
            document->RemoveEventListener(id, &listener, id == Rml::EventId::Focus);
        }
        view.context().UnloadDocument(document);
        document = nullptr;
        chipsRow = nullptr;
        chipsBox = nullptr;
    }

    /// The fades at the edges of the chips row, where chips hide beyond it (the row scrolls with the focus): classes on
    /// the box around the row, set only when they change. Called after the context's update, when the row's layout and
    /// scroll position are current. The chips' boxes decide, not how far the row could still scroll: a chip's margin
    /// keeps the row scrollable by a few pixels while every chip is whole. Reads them again only when the row scrolled
    /// or changed size; allocates nothing.
    void updateChipsCue() {
        if (chipsRow == nullptr || chipsBox == nullptr) {
            return;
        }
        const glm::vec3 row(chipsRow->GetScrollLeft(), chipsRow->GetScrollWidth(), chipsRow->GetClientWidth());
        if (chipsRowSeen && *chipsRowSeen == row) {
            return;
        }
        chipsRowSeen = row;
        constexpr float kSlack = 1.0f; // pixels: rounding of the layout and the scroll position
        const float rowLeft = chipsRow->GetAbsoluteOffset(Rml::BoxArea::Border).x;
        const float rowRight = rowLeft + chipsRow->GetBox().GetSize(Rml::BoxArea::Border).x;
        bool before = false;
        bool after = false;
        for (int index = 0; index < chipsRow->GetNumChildren(); ++index) {
            Rml::Element* chip = chipsRow->GetChild(index);
            const float width = chip->GetBox().GetSize(Rml::BoxArea::Border).x;
            if (width <= 0.0f) {
                continue; // not laid out
            }
            const float left = chip->GetAbsoluteOffset(Rml::BoxArea::Border).x;
            before = before || left < rowLeft - kSlack;
            after = after || left + width > rowRight + kSlack;
        }
        if (before != chipsHiddenBefore) {
            chipsHiddenBefore = before;
            chipsBox->SetClass(moreBeforeClass, before);
        }
        if (after != chipsHiddenAfter) {
            chipsHiddenAfter = after;
            chipsBox->SetClass(moreAfterClass, after);
        }
    }

    // --- Focus -----------------------------------------------------------------------------------------------------

    bool focusElement(std::string_view id) {
        Rml::Element* element = id.empty() ? nullptr : view.element(id);
        if (element == nullptr || document == nullptr || element->GetOwnerDocument() != document) {
            return false;
        }
        if (!element->Focus(true)) {
            return false;
        }
        element->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        return true;
    }

    /// Focuses where the focus lands in `region` (remembered, else entry), else the first focusable choice of the
    /// region (the first visible game, the first chip).
    bool focusRegion(std::string_view region) {
        if (const std::optional<std::string> landing = regions.landing(region); landing && focusElement(*landing)) {
            return true;
        }
        if (region == qc::kRegionList) {
            return !model.visible().empty() && focusElement(gameElementId(model.entries()[model.visible().front()].id));
        }
        if (region == qc::kRegionChips) {
            return focusElement(chipElementId(0));
        }
        if (region == qc::kRegionActions) {
            return focusElement(qc::kPlayId);
        }
        return focusElement(qc::kSearchId);
    }

    /// The list, else the chips: where a screen without focus starts.
    void focusDefault() {
        if (!focusRegion(qc::kRegionList)) {
            static_cast<void>(focusRegion(qc::kRegionChips));
        }
    }

    void onFocus(Rml::Element* element) {
        // RmlUi sends the focus event to every element of the new focus chain, the ancestors of the focused element
        // too: only the elements navigation can focus (tab-index: auto) count.
        if (element == nullptr || element->GetComputedValues().tab_index() != Rml::Style::TabIndex::Auto) {
            return;
        }
        const Rml::String region = regionOf(element);
        const Rml::String& id = element->GetId();
        if (region.empty() || id.empty()) {
            return; // the document itself, or an element outside the regions
        }
        FocusTarget now{region, id};
        if (focus && *focus == now) {
            return;
        }
        history.onFocusMoved(focus, region);
        regions.onFocused(now);
        focus = now;
        if (region == qc::kRegionList && id.starts_with(qc::kGamePrefix)) {
            if (model.select(std::string_view(id).substr(qc::kGamePrefix.size()))) {
                syncSelection();
            }
        } else {
            settleCardNow(); // out of the list: the card's actions act on the selected game, the card shows it
        }
        if (events.focusChanged) {
            events.focusChanged(now);
        }
    }

    void onEvent(Rml::Event& event) {
        Rml::Element* target = event.GetTargetElement();
        switch (event.GetId()) {
        case Rml::EventId::Focus:
            onFocus(target);
            break;
        case Rml::EventId::Dblclick:
            if (regionOf(target) == qc::kRegionList) {
                launch();
            }
            break;
        case Rml::EventId::Change:
            // Enter in the search field: the search is done, on to the results.
            if (target != nullptr && target->GetId() == qc::kSearchId && event.GetParameter("linebreak", false)) {
                static_cast<void>(focusRegion(qc::kRegionList));
            }
            break;
        default:
            break;
        }
    }

    void navigate(NavDirection direction) {
        // A click on the screen's text or background can leave RmlUi's focus off the navigable elements: the first
        // move brings the focus back where the screen had it (it shows where the user is), the next ones move it.
        const Rml::Element* leaf = view.focusedElement();
        if (!focus || leaf == nullptr || leaf->GetId() != focus->element) {
            if (!focus || !focusElement(focus->element)) {
                focusDefault();
            }
            return;
        }
        if (view.navigate(direction) != NavOutcome::Unhandled) {
            return; // moved within the region, or the search field's caret moved
        }
        if (const std::optional<std::string> to = regions.linked(focus->region, direction)) {
            static_cast<void>(focusRegion(*to));
        }
    }

    void back() {
        if (std::optional<FocusTarget> target = history.back()) {
            if (!focusElement(target->element)) {
                static_cast<void>(focusRegion(target->region));
            }
            return;
        }
        if (events.close) {
            events.close();
        }
    }

    void activate() {
        if (focus && focus->region == qc::kRegionList) {
            launch(); // A on a game: Avvia (specification §12.1, "Gioca subito")
            return;
        }
        static_cast<void>(view.activate());
    }

    void apply(UiCommand command) {
        if (!visible || document == nullptr) {
            return;
        }
        if (const std::optional<NavDirection> direction = directionOf(command)) {
            navigate(*direction);
            return;
        }
        switch (command) {
        case UiCommand::Activate:
            activate();
            break;
        case UiCommand::Back:
            back();
            break;
        case UiCommand::Media:
            media();
            break;
        case UiCommand::Info:
            info();
            break;
        default:
            break;
        }
    }

    UiView& view;
    QuickCatalogModel& model;
    QuickCatalogEvents events;
    Listener listener;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    std::vector<ChipRow> chips;
    std::vector<GameRow> games;
    CardData card;
    Rml::String status;
    int count = 0;
    std::optional<std::uint64_t> shownFilter;
    std::uint64_t shownVisible = 0;
    std::uint64_t shownSelection = 0;
    FocusRegions regions;
    FocusHistory history;
    std::optional<FocusTarget> focus;
    bool visible = true;
    bool highContrast = false;
    std::optional<core::FileWatchSet> watch;
    /// When the selected game last changed (UiSystem::time), and whether the card still shows an earlier one.
    std::optional<double> lastSelectionChange;
    bool cardPending = false;
    std::uint64_t cardRefreshes = 0;
    /// The chips row and the box around it (null without a document), the cue classes as set on the box, and their
    /// names (built once: no string is made per frame).
    Rml::Element* chipsRow = nullptr;
    Rml::Element* chipsBox = nullptr;
    /// The row's scroll position, scroll width and client width when the fades were last decided.
    std::optional<glm::vec3> chipsRowSeen;
    bool chipsHiddenBefore = false;
    bool chipsHiddenAfter = false;
    const Rml::String moreBeforeClass{qc::kMoreBeforeClass};
    const Rml::String moreAfterClass{qc::kMoreAfterClass};
};

QuickCatalogScreen::QuickCatalogScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

QuickCatalogScreen::~QuickCatalogScreen() = default;

Expected<std::unique_ptr<QuickCatalogScreen>> QuickCatalogScreen::create(UiView& view, QuickCatalogModel& model,
                                                                         QuickCatalogEvents events, bool watchFiles) {
    auto impl = std::make_unique<Impl>(view, model, std::move(events));
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    impl->regions.link(qc::kRegionSearch, NavDirection::Down, qc::kRegionChips);
    impl->regions.link(qc::kRegionChips, NavDirection::Up, qc::kRegionSearch);
    impl->regions.link(qc::kRegionChips, NavDirection::Down, qc::kRegionList);
    impl->regions.link(qc::kRegionList, NavDirection::Up, qc::kRegionChips);
    impl->regions.link(qc::kRegionList, NavDirection::Right, qc::kRegionActions);
    impl->regions.link(qc::kRegionActions, NavDirection::Left, qc::kRegionList);
    impl->regions.setEntry(qc::kRegionSearch, qc::kSearchId);
    impl->regions.setEntry(qc::kRegionActions, qc::kPlayId);
    impl->syncList();
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    impl->focusDefault();
    if (watchFiles) {
        const std::filesystem::path root = view.system().assetRoot();
        impl->watch.emplace();
        impl->watch->add(root / core::pathFromUtf8(qc::kDocumentPath));
        impl->watch->add(root / core::pathFromUtf8(qc::kStyleSheetPath));
    }
    spdlog::info("Quick catalog: {} game(s), {} filter(s){}", model.entries().size(), model.filters().size(),
                 watchFiles ? ", reloaded when its files change" : "");
    return std::unique_ptr<QuickCatalogScreen>(new QuickCatalogScreen(std::move(impl)));
}

void QuickCatalogScreen::apply(const UiCommandList& commands) {
    for (const UiCommand command : commands.commands()) {
        m_impl->apply(command);
    }
}

void QuickCatalogScreen::apply(UiCommand command) {
    m_impl->apply(command);
}

void QuickCatalogScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (!impl.visible || impl.document == nullptr) {
        return;
    }
    impl.settleCard();
    impl.updateChipsCue();
    // The focused element left the document (its game was filtered out): the focus returns to its region.
    if (impl.view.focusedElement() == nullptr) {
        const std::string region = impl.focus ? impl.focus->region : std::string(qc::kRegionList);
        impl.focus.reset();
        if (!impl.focusRegion(region)) {
            impl.focusDefault();
        }
    }
}

void QuickCatalogScreen::show() {
    Impl& impl = *m_impl;
    if (impl.visible) {
        return;
    }
    impl.visible = true;
    if (impl.document != nullptr) {
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        const std::optional<FocusTarget> previous = std::exchange(impl.focus, std::nullopt);
        if (!previous || !impl.focusElement(previous->element)) {
            impl.focusDefault();
        }
    }
}

void QuickCatalogScreen::hide() {
    Impl& impl = *m_impl;
    if (!impl.visible) {
        return;
    }
    impl.visible = false;
    if (impl.document != nullptr) {
        impl.document->Hide();
    }
}

bool QuickCatalogScreen::visible() const noexcept {
    return m_impl->visible;
}

std::optional<FocusTarget> QuickCatalogScreen::focus() const {
    return m_impl->focus;
}

bool QuickCatalogScreen::focusElement(std::string_view id) {
    return m_impl->focusElement(id);
}

void QuickCatalogScreen::setStatus(std::string_view text) {
    if (m_impl->status != text) {
        m_impl->status = Rml::String(text);
        m_impl->handle.DirtyVariable("status");
    }
}

void QuickCatalogScreen::setHighContrast(bool enabled) {
    m_impl->highContrast = enabled;
    if (m_impl->document != nullptr) {
        m_impl->document->SetClass(Rml::String(qc::kHighContrastClass), enabled);
    }
}

const FocusHistory& QuickCatalogScreen::history() const noexcept {
    return m_impl->history;
}

std::uint64_t QuickCatalogScreen::cardRefreshes() const noexcept {
    return m_impl->cardRefreshes;
}

bool QuickCatalogScreen::reloadIfChanged() {
    Impl& impl = *m_impl;
    if (!impl.watch) {
        return false;
    }
    const std::vector<std::filesystem::path> changed = impl.watch->changed();
    if (changed.empty()) {
        return false;
    }
    spdlog::info("Quick catalog: {} changed on disk, reloading", core::pathToGenericUtf8(changed.front().filename()));
    const std::optional<FocusTarget> previous = std::exchange(impl.focus, std::nullopt);
    impl.unloadDocument();
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    if (auto loaded = impl.loadDocument(); !loaded) {
        spdlog::error("Quick catalog: {}", loaded.error().toString());
        return false;
    }
    if (!previous || !impl.focusElement(previous->element)) {
        impl.focusDefault();
    }
    return true;
}

} // namespace rv::ui
