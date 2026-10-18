#include <rv/ui/quick_catalog_screen.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <RmlUi/Core/Box.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/ScrollTypes.h>
#include <RmlUi/Core/Unit.h>
#include <RmlUi/Core/Variant.h>
#include <glm/vec3.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/file_watch_set.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/filter_count_log.hpp>
#include <rv/ui/focus_regions.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

namespace qc = quick_catalog;

constexpr std::size_t kNone = static_cast<std::size_t>(-1);
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// A filter chip as the document binds it. The chip of every game has an empty label: the document names it.
struct ChipRow {
    Rml::String elementId;
    Rml::String key;
    Rml::String label;
    bool active = false;
};

/// A row of the pool as the document binds it: the game its slot shows, or a blank row (past the list's end, or a
/// window the source could not read).
struct RowData {
    Rml::String elementId;
    bool present = false; ///< the slot shows a game (else the row is blank)
    Rml::String title;
    Rml::String platform;
    Rml::String year; ///< empty when unknown: the document writes "n.d."
    Rml::String state;
};

/// What a slot of the pool shows: a position of the list and its game's id (kNone and empty: a blank row).
struct SlotShown {
    std::size_t position = kNone;
    std::string id;
};

/// The selected game's card. Unknown fields are empty: the document writes "n.d.".
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
    Rml::String state;
    /// CatalogEntry::readiness, blocker, blockerDetail and verified: the launch readiness, its first blocker's code and
    /// sentence ("Dettagli"), and whether the game's command returned normally once.
    Rml::String readiness;
    Rml::String blocker;
    Rml::String detail;
    bool verified = false;
};

/// The scroll indicator beside the list: its thumb's top and height in percent of the track.
struct ScrollData {
    bool shown = false;
    Rml::String top;
    Rml::String height;
};

/// "Aggiorna libreria" and the line under the header (QuickCatalogRefresh, bound as the data model's "refresh").
struct RefreshData {
    bool available = false;
    Rml::String state;
    Rml::String phase;
    int done = 0;
    int total = 0;
    int added = 0;
    int removed = 0;
    int changed = 0;
    int guarded = 0;
};

[[nodiscard]] int countOf(std::size_t value) noexcept {
    return static_cast<int>(std::min<std::size_t>(value, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

[[nodiscard]] Rml::String yearText(int year) {
    return year > 0 ? Rml::String(std::to_string(year)) : Rml::String();
}

[[nodiscard]] Rml::String chipElementId(std::size_t index) {
    return Rml::String(qc::kChipPrefix) + std::to_string(index);
}

/// The slot of a row's element id ("row-3": 3), if it is one.
[[nodiscard]] std::optional<std::size_t> slotOf(std::string_view id) noexcept {
    if (!id.starts_with(qc::kRowPrefix)) {
        return std::nullopt;
    }
    const std::string_view digits = id.substr(qc::kRowPrefix.size());
    std::size_t slot = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), slot);
    if (error != std::errc() || end != digits.data() + digits.size() || slot >= qc::kPoolRows) {
        return std::nullopt;
    }
    return slot;
}

/// The row of the window the slot `slot` of a pool of `shown` rows shows while the window starts at `first`: the slot
/// of position p is p mod shown (quick_catalog::poolSlotOf).
[[nodiscard]] std::size_t windowRowOf(std::size_t slot, std::size_t first, std::size_t shown) noexcept {
    return (slot + shown - first % shown) % shown;
}

/// "12.345%": a percentage for a style property, without allocating (it fits the string's own storage).
void setPercent(Rml::String& text, float percent) {
    std::array<char, 24> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size() - 1, percent, std::chars_format::fixed, 3);
    char* last = error == std::errc() ? end : buffer.data();
    *last++ = '%';
    text.assign(buffer.data(), last);
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

/// Games the selection moves per notch of the mouse wheel over the list.
constexpr std::ptrdiff_t kWheelRows = 3;

/// The scroll indicator's thumb is at least this tall (percent of the track), so that it shows on a long list.
constexpr float kMinThumbPercent = 4.0f;

/// Events the screen listens to on its document: every focus change (capture phase: focus does not bubble), double
/// clicks on games, the line break that ends a search, the mouse wheel over the list.
constexpr std::array<Rml::EventId, 4> kListenedEvents{Rml::EventId::Focus, Rml::EventId::Dblclick, Rml::EventId::Change,
                                                      Rml::EventId::Mousescroll};

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
        : view(view), model(model), events(std::move(events)), listener(*this) {
        rows.reserve(qc::kPoolRows); // resized with the rows that fit, never moved: no allocation of its own after this
        for (std::size_t slot = 0; slot < qc::kPoolRows; ++slot) {
            rowIds[slot] = Rml::String(qc::kRowPrefix) + std::to_string(slot);
        }
        appliedTops.fill(kNaN);
    }
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
        if (auto row = constructor.RegisterStruct<RowData>()) {
            row.RegisterMember("elementId", &RowData::elementId);
            row.RegisterMember("present", &RowData::present);
            row.RegisterMember("title", &RowData::title);
            row.RegisterMember("platform", &RowData::platform);
            row.RegisterMember("year", &RowData::year);
            row.RegisterMember("state", &RowData::state);
        }
        constructor.RegisterArray<std::vector<RowData>>();
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
            cardStruct.RegisterMember("state", &CardData::state);
            cardStruct.RegisterMember("readiness", &CardData::readiness);
            cardStruct.RegisterMember("blocker", &CardData::blocker);
            cardStruct.RegisterMember("detail", &CardData::detail);
            cardStruct.RegisterMember("verified", &CardData::verified);
        }
        if (auto scrollStruct = constructor.RegisterStruct<ScrollData>()) {
            scrollStruct.RegisterMember("shown", &ScrollData::shown);
            scrollStruct.RegisterMember("top", &ScrollData::top);
            scrollStruct.RegisterMember("height", &ScrollData::height);
        }
        if (auto refreshStruct = constructor.RegisterStruct<RefreshData>()) {
            refreshStruct.RegisterMember("available", &RefreshData::available);
            refreshStruct.RegisterMember("state", &RefreshData::state);
            refreshStruct.RegisterMember("phase", &RefreshData::phase);
            refreshStruct.RegisterMember("done", &RefreshData::done);
            refreshStruct.RegisterMember("total", &RefreshData::total);
            refreshStruct.RegisterMember("added", &RefreshData::added);
            refreshStruct.RegisterMember("removed", &RefreshData::removed);
            refreshStruct.RegisterMember("changed", &RefreshData::changed);
            refreshStruct.RegisterMember("guarded", &RefreshData::guarded);
        }
        constructor.Bind("chips", &chips);
        constructor.Bind("rows", &rows);
        constructor.Bind("card", &card);
        constructor.Bind("scroll", &scroll);
        constructor.Bind("refresh", &refreshData);
        constructor.Bind("status", &status);
        constructor.Bind("count", &count);
        constructor.Bind("unavailable", &unavailable);
        constructor.Bind("notice", &notice);
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
        constructor.BindEventCallback("refresh", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (refreshShown.available && events.refresh) {
                events.refresh();
            }
        });
        handle = constructor.GetModelHandle();
        return ok();
    }

    /// Everything the model changed since the last call: the chips, the window's rows, the card, the selected row's
    /// class.
    void sync() {
        syncChips();
        syncWindow();
        syncSelection();
        syncSelectedRow();
    }

    /// Shows what the model holds when it changed outside the screen's own commands: a list a query running on the
    /// source's worker brought (QuickCatalogModel::poll), the application's QuickCatalogModel::refresh() after an
    /// import. Before every command and after every update, so that a command never acts on a game the screen does not
    /// show; nothing to do when the model did not change.
    void catchUp() {
        if (shownFilter == model.filterRevision() && shownWindow == model.windowRevision() &&
            shownSelection == model.selectionRevision() && shownContent == model.contentRevision()) {
            return;
        }
        logError("catalog");
        sync();
        followSelection();
    }

    /// The chips, their active flags and the chips region's entry (the active chip), when the filters or the active
    /// filter changed.
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

    /// The pool's rows, the count, the empty list's reason and the scroll indicator, when the model's window changed.
    /// The pool has a row per window row; a game keeps its row while the window holds it (the slot of position p is p
    /// mod the rows shown), so a scroll binds only the rows that enter (one per step of a held direction: one row's
    /// text changes instead of the window's), and the rows are moved into the window's order (placeRows).
    void syncWindow() {
        if (shownWindow && *shownWindow == model.windowRevision()) {
            return;
        }
        shownWindow = model.windowRevision();
        const std::size_t shown = std::min(model.windowRows(), qc::kPoolRows);
        bool bound = false;
        if (rows.size() != shown) {
            resizePool(shown);
            bound = true;
        }
        // Another list binds every row (the same position may hold another game, the same game other data), and so do
        // rows the source changed in place (QuickCatalogModel::reread).
        const bool newList = !boundList || *boundList != model.listRevision() || boundContent != model.contentRevision();
        boundList = model.listRevision();
        boundContent = model.contentRevision();
        const std::span<const CatalogEntry> window = model.window();
        const std::size_t first = model.windowFirst();
        for (std::size_t slot = 0; slot < shown; ++slot) {
            const std::size_t row = windowRowOf(slot, first, shown);
            const CatalogEntry* entry = row < window.size() ? &window[row] : nullptr;
            const std::size_t position = entry != nullptr ? first + row : kNone;
            const SlotShown& was = slots[slot];
            if (!newList && was.position == position && (entry != nullptr ? was.id == entry->id : was.id.empty())) {
                continue; // still in the window: the row shows it already
            }
            bindRow(slot, position, entry);
            bound = true;
        }
        if (bound) {
            ++windowBinds;
            handle.DirtyVariable("rows");
        }
        placeRows();
        const int listed = static_cast<int>(std::min<std::size_t>(model.count(), std::numeric_limits<int>::max()));
        if (listed != count) {
            count = listed;
            handle.DirtyVariable("count");
        }
        // The list shows nothing because the source could not be read, not because nothing matches.
        const bool cannotRead = model.error().has_value() && window.empty();
        if (cannotRead != unavailable) {
            unavailable = cannotRead;
            handle.DirtyVariable("unavailable");
        }
        syncScroll();
    }

    /// The pool as tall as the rows that fit: the data-for view makes or removes rows at the context's next update,
    /// when attachPool() finds them; every row is bound again.
    void resizePool(std::size_t shown) {
        if (markedSlot && *markedSlot >= shown) {
            if (rowElements[*markedSlot] != nullptr) {
                rowElements[*markedSlot]->SetClass(selectedClass, false);
            }
            markedSlot.reset();
        }
        for (std::size_t slot = shown; slot < qc::kPoolRows; ++slot) {
            rowElements[slot] = nullptr;
            slotBoxes[slot] = nullptr;
        }
        const std::size_t before = rows.size();
        rows.resize(shown);
        for (std::size_t slot = before; slot < shown; ++slot) {
            rows[slot].elementId = rowIds[slot];
        }
        slots.fill(SlotShown{});
        boundList.reset();
        poolStale = true;
    }

    /// Binds the slot `slot` to the game `entry` at `position` (nothing: a blank row).
    void bindRow(std::size_t slot, std::size_t position, const CatalogEntry* entry) {
        RowData& row = rows[slot];
        row.present = entry != nullptr;
        if (entry != nullptr) {
            row.title = entry->title;
            row.platform = entry->platform;
            row.year = yearText(entry->year);
            row.state = entry->state;
        } else {
            row.title.clear();
            row.platform.clear();
            row.year.clear();
            row.state.clear();
        }
        slots[slot].position = position;
        if (entry != nullptr) {
            slots[slot].id.assign(entry->id); // keeps its storage
        } else {
            slots[slot].id.clear();
        }
        ++rowBinds;
    }

    /// Moves each row of the pool from its place in the list's flow to its row of the window (style top, relative, in
    /// whole pixels so that the rows' rounding never changes: their geometry is moved, not built again); sets only the
    /// offsets that changed.
    void placeRows() {
        const std::size_t shown = rows.size();
        const std::size_t first = model.windowFirst();
        for (std::size_t slot = 0; slot < shown; ++slot) {
            Rml::Element* element = rowElements[slot];
            if (element == nullptr) {
                continue;
            }
            const auto offset = static_cast<float>(windowRowOf(slot, first, shown)) - static_cast<float>(slot);
            const float top = std::round(offset * rowPitch);
            if (top != appliedTops[slot]) { // a NaN (not set yet) never compares equal
                element->SetProperty(Rml::PropertyId::Top, Rml::Property(top, Rml::Unit::PX));
                appliedTops[slot] = top;
            }
        }
    }

    /// After a context update that made the rows of a resized pool or of a loaded document: finds them, places them,
    /// marks the selected one and gives the focus to it when it waited for its row.
    void attachPool() {
        if (!poolStale || document == nullptr) {
            return;
        }
        bool found = true;
        for (std::size_t slot = 0; slot < qc::kPoolRows; ++slot) {
            Rml::Element* element = slot < rows.size() ? document->GetElementById(rowIds[slot]) : nullptr;
            found = found && (slot >= rows.size() || element != nullptr);
            rowElements[slot] = element;
            slotBoxes[slot] = element != nullptr ? element->GetParentNode() : nullptr;
            appliedTops[slot] = kNaN;
            if (element != nullptr) {
                element->SetClass(selectedClass, false); // a row kept from a larger pool may have had it
            }
        }
        poolStale = !found;
        markedSlot.reset();
        placeRows();
        syncSelectedRow();
        if (!poolStale && std::exchange(focusWaits, false)) {
            followSelection();
        }
    }

    /// The scroll indicator: shown while the list holds more games than the window, its thumb as tall as the window's
    /// share of the list (at least kMinThumbPercent) and as far down the track as the window is down the list.
    void syncScroll() {
        const std::size_t total = model.count();
        const std::size_t shownRows = model.windowRows();
        const bool shown = total > shownRows;
        float height = 100.0f;
        float top = 0.0f;
        if (shown) {
            height = std::max(100.0f * static_cast<float>(shownRows) / static_cast<float>(total), kMinThumbPercent);
            top = (100.0f - height) * static_cast<float>(model.windowFirst()) / static_cast<float>(total - shownRows);
        }
        scroll.shown = shown;
        setPercent(scroll.top, top);
        setPercent(scroll.height, height);
        handle.DirtyVariable("scroll");
    }

    /// The status line and the card when the selected game changed.
    void syncSelection() {
        if (shownSelection && *shownSelection == model.selectionRevision()) {
            // The same game, other data (QuickCatalogModel::reread): the card shows it at once, the status line stays.
            if (shownContent != model.contentRevision()) {
                shownContent = model.contentRevision();
                refreshCard();
            }
            return;
        }
        shownSelection = model.selectionRevision();
        shownContent = model.contentRevision();
        // The status line speaks of the game it was set for: another game clears it.
        if (!status.empty()) {
            status.clear();
            handle.DirtyVariable("status");
        }
        // The card follows at once after a pause, and once the selection rests while it runs through the list (a held
        // direction): laying the card's text out again is most of a move's cost (docs/spikes/i1-ui.md).
        const double now = view.system().time();
        const bool resting = !lastSelectionChange || now - *lastSelectionChange >= kCardSettleSeconds;
        lastSelectionChange = now;
        if (resting || model.selected() == nullptr) {
            refreshCard();
        } else {
            cardPending = true;
        }
    }

    /// The slot whose row shows the selected game, if the pool shows it.
    [[nodiscard]] std::optional<std::size_t> selectedPoolSlot() const noexcept {
        const std::optional<std::size_t> position = model.selectedPosition();
        if (!position || rows.empty()) {
            return std::nullopt;
        }
        const std::size_t slot = qc::poolSlotOf(*position, rows.size());
        return slots[slot].position == *position ? std::optional<std::size_t>(slot) : std::nullopt;
    }

    /// The class of the selected game's row, moved from the row that had it: two elements change, the bound rows are
    /// not evaluated again (BACKLOG "Per-change cost under 1 ms").
    void syncSelectedRow() {
        const std::optional<std::size_t> slot = selectedPoolSlot();
        if (slot == markedSlot) {
            return;
        }
        if (markedSlot && rowElements[*markedSlot] != nullptr) {
            rowElements[*markedSlot]->SetClass(selectedClass, false);
        }
        markedSlot.reset();
        if (slot && rowElements[*slot] != nullptr) {
            rowElements[*slot]->SetClass(selectedClass, true);
            markedSlot = slot; // without its element (not made yet) attachPool() marks it
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
            card.platform = selected->platform;
            card.year = yearText(selected->year);
            card.developer = selected->developer;
            card.publisher = selected->publisher;
            card.genre = selected->genre;
            card.players = selected->players;
            card.description = selected->description;
            card.state = selected->state;
            card.readiness = selected->readiness;
            card.blocker = selected->blocker;
            card.detail = selected->blockerDetail;
            card.verified = selected->verified;
        }
        handle.DirtyVariable("card");
    }

    /// Refreshes a card left behind by quick moves once the selection has rested for kCardSettleSeconds.
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
        logError("search");
        sync();
        followSelection();
    }

    void applyFilter(const std::string& key) {
        if (!model.setFilter(key)) {
            spdlog::warn("Quick catalog: unknown filter '{}'", key);
            return;
        }
        logError("filter");
        noteFilter(model.activeFilter());
        sync();
        followSelection();
    }

    /// A filter became the active one: its count is logged once its list is the one shown (at once when the source
    /// answered at once, else when afterUpdate() collects it from the catalog worker).
    void noteFilter(const std::string& key) {
        filterLog.chosen(key);
        if (model.pending()) {
            spdlog::debug("Quick catalog: filter '{}' chosen, its list comes from the catalog's worker",
                          key.empty() ? "(every game)" : key);
        }
        logFilterCount();
    }

    /// Logs how many games the chosen filter lets through once its list is the one shown (FilterCountLog).
    void logFilterCount() {
        if (const std::optional<std::string> line = filterLog.take(model)) {
            spdlog::info("Quick catalog: {}", *line);
        }
    }

    /// The shoulders: the next or previous filter from any region. The active chip comes into view (the focus moves to
    /// it while it is in the chips row).
    void cycleFilter(int step) {
        if (!model.cycleFilter(step)) {
            return;
        }
        logError("filter");
        noteFilter(model.activeFilter());
        sync();
        const std::optional<std::string> active = regions.entry(qc::kRegionChips);
        if (focus && focus->region == qc::kRegionChips && active) {
            static_cast<void>(focusElement(*active));
        } else if (Rml::Element* chip = active ? view.element(*active) : nullptr) {
            chip->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        }
        followSelection();
    }

    /// Logs the model's error once per failure (the list is then empty).
    void logError(std::string_view what) {
        if (model.error() && (!loggedError || loggedError->message != model.error()->message)) {
            spdlog::warn("Quick catalog: the {} could not be read: {}", what, model.error()->toString());
        }
        loggedError = model.error();
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
        // Generates the rows of the data views, so that the focus can land on them, and lays them out for the window's
        // first measurement.
        poolStale = true;
        view.context().Update();
        chipsRow = document->GetElementById(Rml::String(qc::kChipsId));
        chipsBox = document->GetElementById(Rml::String(qc::kChipsBoxId));
        list = document->GetElementById(Rml::String(qc::kListId));
        attachPool();
        chipsHiddenBefore = false;
        chipsHiddenAfter = false;
        chipsRowSeen.reset();
        updateChipsCue();
        // The window as tall as the list area, laid out before the first frame.
        if (measureRows() || poolStale) {
            view.context().Update();
            attachPool();
        }
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
        list = nullptr;
        rowElements.fill(nullptr);
        slotBoxes.fill(nullptr);
        markedSlot.reset();
        poolStale = true;
    }

    /// The rows that fit the list area, from the laid-out rows: the pitch between the first two places of the list's
    /// flow (the rows' slots: a row and its spacing), the height of a row, the height of the list's content box. Gives
    /// the model a window that tall when it changed (a resize, the text scale, the layout theme), and moves the rows to
    /// a new pitch. True when the rows must be laid out again before this frame is drawn. The list's height does not
    /// depend on its rows, so the next layout measures the same. Reads boxes only: no allocation.
    bool measureRows() {
        if (list == nullptr || poolStale || rows.empty() || rowElements[0] == nullptr) {
            return false;
        }
        bool moved = false;
        if (rows.size() >= 2 && slotBoxes[0] != nullptr && slotBoxes[1] != nullptr) {
            const float top = slotBoxes[0]->GetAbsoluteOffset(Rml::BoxArea::Border).y;
            const float next = slotBoxes[1]->GetAbsoluteOffset(Rml::BoxArea::Border).y;
            const float height = rowElements[0]->GetBox().GetSize(Rml::BoxArea::Border).y;
            if (next > top && height > 0.0f && (next - top != rowPitch || height != rowHeight)) {
                rowPitch = next - top;
                rowHeight = height;
                placeRows();
                moved = true;
            }
        }
        if (rowPitch <= 0.0f || rowHeight <= 0.0f) {
            return moved;
        }
        const float area = list->GetBox().GetSize(Rml::BoxArea::Content).y;
        // A hundredth of a pixel of slack: rows that fill the area exactly must count as fitting.
        const float spare = (area - rowHeight) / rowPitch + 0.01f;
        const std::size_t fit = spare >= 0.0f ? static_cast<std::size_t>(spare) + 1 : 1;
        const std::size_t rowsShown = std::clamp<std::size_t>(fit, 1, qc::kPoolRows);
        if (rowsShown == model.windowRows()) {
            return moved;
        }
        model.setWindowRows(rowsShown);
        logError("list");
        sync();
        followSelection(); // waits for its row when the pool grew past it
        return true;
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

    /// Focuses the selected game's row; false when the list is empty (or the row is not made yet).
    bool focusSelectedRow() {
        const std::optional<std::size_t> slot = selectedPoolSlot();
        return slot && model.selected() != nullptr && focusElement(rowIds[*slot]);
    }

    /// The active filter's chip: where the focus goes when the list it was in, or the card's actions it was on, empty
    /// (the filter that emptied the list, where the next one is a shoulder or a step away).
    void focusActiveChip() {
        const std::optional<std::string> active = regions.entry(qc::kRegionChips);
        if (!active || !focusElement(*active)) {
            static_cast<void>(focusRegion(qc::kRegionChips));
        }
    }

    /// The focus follows the selection: while the list has it, it moves to the selected game's row (each game keeps its
    /// row while the window holds it); an empty list sends it to the filters, and so does no game selected while it is
    /// on the card's actions (the card is hidden then: the focus would stay on a button nobody sees).
    void followSelection() {
        if (!focus) {
            return;
        }
        if (focus->region == qc::kRegionActions && model.selected() == nullptr) {
            focusActiveChip();
            return;
        }
        if (focus->region != qc::kRegionList) {
            return;
        }
        const std::optional<std::size_t> slot = selectedPoolSlot();
        if (slot && model.selected() != nullptr && rowElements[*slot] != nullptr &&
            view.focusedElement() == rowElements[*slot]) {
            return; // on it already
        }
        if (focusSelectedRow()) {
            return;
        }
        if (poolStale && model.selected() != nullptr) {
            focusWaits = true; // the row is made at the next update (attachPool)
            return;
        }
        focusActiveChip();
    }

    /// Focuses where the focus lands in `region`: the selected game in the list; elsewhere the remembered element, else
    /// the entry, else the region's first choice (the first chip, Avvia, the search field).
    bool focusRegion(std::string_view region) {
        if (region == qc::kRegionList) {
            return focusSelectedRow();
        }
        if (region == qc::kRegionLibrary) {
            // "Aggiorna libreria" shows only while the catalog can refresh.
            return refreshShown.available && focusElement(qc::kRefreshId);
        }
        if (const std::optional<std::string> landing = regions.landing(region); landing && focusElement(*landing)) {
            return true;
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

    /// Focuses `target` again: the selected game for the list (its row may show another game by now).
    bool refocus(const FocusTarget& target) {
        return target.region == qc::kRegionList ? focusSelectedRow() : focusElement(target.element);
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
        if (region == qc::kRegionList) {
            // A row: the game it shows becomes the selected one (a click; the screen's own focus follows the selection
            // already). A blank row (focused with Tab), or a game gone from a list the screen has not shown yet, sends
            // the focus back to the selected game.
            const std::optional<std::size_t> slot = slotOf(id);
            if (!slot || *slot >= rows.size() || slots[*slot].position == kNone) {
                static_cast<void>(focusSelectedRow());
                return;
            }
            const SlotShown& shown = slots[*slot];
            const bool current = boundList && *boundList == model.listRevision();
            if (!(current ? model.selectPosition(shown.position) : model.select(shown.id))) {
                static_cast<void>(focusSelectedRow());
                return;
            }
            sync();
        }
        FocusTarget now{region, id};
        if (focus && *focus == now) {
            return;
        }
        history.onFocusMoved(focus, region);
        regions.onFocused(now);
        focus = now;
        if (region != qc::kRegionList) {
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
        case Rml::EventId::Mousescroll:
            // The list does not scroll in RmlUi (its rows are the window): the wheel moves the selection, positive
            // down.
            if (regionOf(target) == qc::kRegionList) {
                const float delta = event.GetParameter("wheel_delta_y", 0.0f);
                if (delta != 0.0f && model.moveSelection(delta > 0.0f ? kWheelRows : -kWheelRows)) {
                    logError("list");
                    sync();
                    followSelection();
                }
                event.StopPropagation();
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
            if (!focus || !refocus(*focus)) {
                focusDefault();
            }
            return;
        }
        if (focus->region == qc::kRegionList) {
            // Up and Down move the selection over the whole list; at its first game Up leaves for the filters (the
            // region link), at its last Down stays. Left and Right leave by the links.
            const bool vertical = direction == NavDirection::Up || direction == NavDirection::Down;
            if (vertical && model.moveSelection(direction == NavDirection::Up ? -1 : 1)) {
                logError("list");
                sync();
                followSelection();
                return;
            }
        } else if (view.navigate(direction) != NavOutcome::Unhandled) {
            return; // moved within the region, or the search field's caret moved
        }
        if (const std::optional<std::string> to = regions.linked(focus->region, direction)) {
            static_cast<void>(focusRegion(*to));
        }
    }

    /// Page moves, Home and End: jumps in the list; Home and End move the search field's caret and go to the first and
    /// last chip.
    void jump(UiCommand command) {
        if (!focus) {
            return;
        }
        if (focus->region == qc::kRegionList) {
            bool moved = false;
            switch (command) {
            case UiCommand::PageUp:
                moved = model.pageUp();
                break;
            case UiCommand::PageDown:
                moved = model.pageDown();
                break;
            case UiCommand::Home:
                moved = model.home();
                break;
            case UiCommand::End:
                moved = model.end();
                break;
            default:
                break;
            }
            if (moved) {
                logError("list");
                sync();
                followSelection();
            }
            return;
        }
        const bool home = command == UiCommand::Home;
        if (!home && command != UiCommand::End) {
            return;
        }
        if (focus->region == qc::kRegionSearch) {
            static_cast<void>(view.caretKey(home ? CaretKey::Home : CaretKey::End));
        } else if (focus->region == qc::kRegionChips && !chips.empty()) {
            static_cast<void>(focusElement(chipElementId(home ? 0 : chips.size() - 1)));
        }
    }

    void back() {
        if (std::optional<FocusTarget> target = history.back()) {
            if (!refocus(*target)) {
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
        catchUp(); // a command acts on the games the screen shows
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
        case UiCommand::PageUp:
        case UiCommand::PageDown:
        case UiCommand::Home:
        case UiCommand::End:
            jump(command);
            break;
        case UiCommand::FilterPrevious:
            cycleFilter(-1);
            break;
        case UiCommand::FilterNext:
            cycleFilter(1);
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
    /// The pool: one row per window row (at most kPoolRows; capacity reserved).
    std::vector<RowData> rows;
    CardData card;
    ScrollData scroll;
    /// The refresh as set (setRefresh) and as bound.
    QuickCatalogRefresh refreshShown;
    RefreshData refreshData;
    Rml::String status;
    int count = 0;
    /// The list is empty because the source could not be read (the document says so instead of "no match").
    bool unavailable = false;
    /// What the list says about the catalog itself (setNotice: quick_catalog::kNotice*); empty: nothing.
    Rml::String notice;
    /// The model's revisions the document shows.
    std::optional<std::uint64_t> shownFilter;
    std::optional<std::uint64_t> shownWindow;
    std::optional<std::uint64_t> shownSelection;
    std::uint64_t shownContent = 0;
    /// The content revision (QuickCatalogModel::reread) the pool's rows were bound with.
    std::uint64_t boundContent = 0;
    /// The list the rows were bound from (QuickCatalogModel::listRevision), and what each slot shows.
    std::optional<std::uint64_t> boundList;
    std::array<SlotShown, qc::kPoolRows> slots;
    std::uint64_t windowBinds = 0;
    std::uint64_t rowBinds = 0;
    /// The last error of the model that was logged.
    std::optional<Error> loggedError;
    /// The filter chosen last, until its count is logged.
    FilterCountLog filterLog;
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
    /// The rows' element ids, built once (no string is made per move), their elements and the elements of their places
    /// in the list's flow (the data-for slots, which never move: null without a document or past the pool), and the
    /// offset each row was moved by (NaN: none set yet).
    std::array<Rml::String, qc::kPoolRows> rowIds;
    std::array<Rml::Element*, qc::kPoolRows> rowElements{};
    std::array<Rml::Element*, qc::kPoolRows> slotBoxes{};
    std::array<float, qc::kPoolRows> appliedTops{};
    /// The pool's rows changed (resized, a document loaded): attachPool() finds them after the next update; the focus
    /// waits for the selected game's row meanwhile.
    bool poolStale = true;
    bool focusWaits = false;
    Rml::Element* list = nullptr;
    /// The row that has the selected class.
    std::optional<std::size_t> markedSlot;
    /// The rows' pitch (a row and its spacing) and height as last measured, in pixels.
    float rowPitch = 0.0f;
    float rowHeight = 0.0f;
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
    const Rml::String selectedClass{qc::kSelectedClass};
};

QuickCatalogScreen::QuickCatalogScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

QuickCatalogScreen::~QuickCatalogScreen() = default;

Expected<std::unique_ptr<QuickCatalogScreen>> QuickCatalogScreen::create(UiView& view, QuickCatalogModel& model,
                                                                         QuickCatalogEvents events, bool watchFiles) {
    auto impl = std::make_unique<Impl>(view, model, std::move(events));
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    impl->regions.link(qc::kRegionLibrary, NavDirection::Down, qc::kRegionSearch);
    impl->regions.link(qc::kRegionSearch, NavDirection::Up, qc::kRegionLibrary);
    impl->regions.setEntry(qc::kRegionLibrary, qc::kRefreshId);
    impl->regions.link(qc::kRegionSearch, NavDirection::Down, qc::kRegionChips);
    impl->regions.link(qc::kRegionChips, NavDirection::Up, qc::kRegionSearch);
    impl->regions.link(qc::kRegionChips, NavDirection::Down, qc::kRegionList);
    impl->regions.link(qc::kRegionList, NavDirection::Up, qc::kRegionChips);
    impl->regions.link(qc::kRegionList, NavDirection::Right, qc::kRegionActions);
    impl->regions.link(qc::kRegionActions, NavDirection::Left, qc::kRegionList);
    impl->regions.setEntry(qc::kRegionSearch, qc::kSearchId);
    impl->regions.setEntry(qc::kRegionActions, qc::kPlayId);
    impl->logError("catalog");
    impl->sync();
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
    spdlog::info("Quick catalog: {} game(s), {} filter(s), {} row(s) shown of a pool of at most {}{}", model.count(),
                 model.filters().size(), model.windowRows(), qc::kPoolRows,
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
    impl.attachPool(); // the rows this update made, when the pool grew
    // A list a query running on the source's worker brought, and what the application changed in the model directly.
    static_cast<void>(impl.model.poll());
    impl.catchUp();
    impl.logFilterCount(); // a filter's list the worker brought: its count, as the screen now shows it
    impl.settleCard();
    impl.updateChipsCue();
    if (impl.measureRows() || impl.poolStale) {
        // The rows' number or pitch changed: laid out again at once, so that this frame shows them in place.
        impl.view.context().Update();
        impl.attachPool();
    }
    // The focused element left the document (a hot reload): the focus returns to its region. One that is no longer
    // visible (a panel hidden around it) gives way to the list, else the filters.
    const Rml::Element* focused = impl.view.focusedElement();
    if (focused == nullptr) {
        const std::string region = impl.focus ? impl.focus->region : std::string(qc::kRegionList);
        impl.focus.reset();
        if (!impl.focusRegion(region)) {
            impl.focusDefault();
        }
    } else if (!focused->IsVisible(true)) {
        impl.focus.reset();
        impl.focusDefault();
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
        impl.catchUp();
        const std::optional<FocusTarget> previous = std::exchange(impl.focus, std::nullopt);
        if (!previous || !impl.refocus(*previous)) {
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

void QuickCatalogScreen::setNotice(std::string_view key) {
    if (m_impl->notice != key) {
        m_impl->notice = Rml::String(key);
        m_impl->handle.DirtyVariable("notice");
    }
}

void QuickCatalogScreen::setRefresh(const QuickCatalogRefresh& refresh) {
    Impl& impl = *m_impl;
    if (impl.refreshShown == refresh) {
        return;
    }
    impl.refreshShown = refresh;
    RefreshData& data = impl.refreshData;
    data.available = refresh.available;
    data.state = Rml::String(refresh.state);
    data.phase = Rml::String(refresh.phase);
    data.done = countOf(refresh.done);
    data.total = countOf(refresh.total);
    data.added = countOf(refresh.added);
    data.removed = countOf(refresh.removed);
    data.changed = countOf(refresh.changed);
    data.guarded = countOf(refresh.guarded);
    impl.handle.DirtyVariable("refresh");
}

const QuickCatalogRefresh& QuickCatalogScreen::refresh() const noexcept {
    return m_impl->refreshShown;
}

std::string_view QuickCatalogScreen::notice() const noexcept {
    return m_impl->notice;
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

std::uint64_t QuickCatalogScreen::windowBinds() const noexcept {
    return m_impl->windowBinds;
}

std::uint64_t QuickCatalogScreen::rowBinds() const noexcept {
    return m_impl->rowBinds;
}

std::size_t QuickCatalogScreen::shownCount() const noexcept {
    return static_cast<std::size_t>(std::max(m_impl->count, 0));
}

std::vector<std::string> QuickCatalogScreen::squeezedCardBlocks() const {
    std::vector<std::string> squeezed;
    Rml::ElementDocument* document = m_impl->document;
    Rml::Element* card = document != nullptr && document->IsVisible()
                             ? document->GetElementById(Rml::String(qc::kCardId))
                             : nullptr;
    if (card == nullptr || !card->IsVisible()) {
        return squeezed;
    }
    for (int index = 0; index < card->GetNumChildren(); ++index) {
        Rml::Element* block = card->GetChild(index);
        if (block == nullptr || !block->IsVisible()) {
            continue;
        }
        // What it wants: its content, at most its max-height (a length: em and dp are resolved when computed) and its
        // padding; scroll heights include the padding as client heights do.
        float wanted = block->GetScrollHeight();
        const Rml::Style::MaxHeight maxHeight = block->GetComputedValues().max_height();
        if (maxHeight.type == Rml::Style::LengthPercentage::Length && maxHeight.value < 1.0e6f) {
            const Rml::Box& box = block->GetBox();
            wanted = std::min(wanted, maxHeight.value + box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) +
                                          box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom));
        }
        if (block->GetClientHeight() + 0.5f < wanted) {
            const Rml::String& id = block->GetId();
            const Rml::String classes = block->GetClassNames();
            squeezed.emplace_back(!id.empty() ? id : classes.substr(0, classes.find(' ')));
        }
    }
    return squeezed;
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
    if (!previous || !impl.refocus(*previous)) {
        impl.focusDefault();
    }
    return true;
}

} // namespace rv::ui
