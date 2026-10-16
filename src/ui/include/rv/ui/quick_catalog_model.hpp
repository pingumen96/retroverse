#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/ui/catalog_source.hpp>

namespace rv::ui {

/// The view model of the quick catalog (specification §12.2 QuickLibrary, §13.2): a window over a CatalogSource. It
/// keeps the filter chips, the search text, the size of the list, the position of the selected game and the rows of the
/// window the view shows (the selected game's among them, which the card shows), and never state per entry of the
/// catalog: the source keeps the list, so 100,000 games cost what the window's rows cost. GPU-free, toolkit-free: the
/// RmlUi screen binds it (rv::ui::QuickCatalogScreen).
///
///  - Filters: every game first (empty key; the documents name its chip), then the source's platforms.
///  - Search: the text the source matches.
///  - Selection: a position of the list, none while the list is empty. A new list keeps it on its game when the list
///    still holds the game; when it does not, a new search or filter starts at the first entry, and refresh() (the same
///    query materialized again: the catalog changed) takes the nearest position, the one the game had (or the last).
///  - Window: windowRows() positions from windowFirst(); the selection always lies inside. A move scrolls it only when
///    the selection would come closer than the scroll margin to the edge it moves towards (the list's ends aside), a
///    page scrolls it by a page with the selection, and a new list or another window size keeps the selection's row
///    where it was, within the margins. A scroll reads only the rows that enter the window (one per step of a held
///    direction); a new list or window size reads the whole window.
///  - Lists the source materializes on its worker (QueryState::Pending): the model keeps the list it has (moves, pages
///    and the window work on it) until poll() collects the new one, then places the selection as the operation that
///    asked for it says (when a search or a filter asked since the last list, a game the new list lacks restarts at
///    its first entry). Only the latest query's list is applied: typing coalesces.
/// The revisions count the changes, so that a view refreshes only what changed. Errors of the source are kept in
/// error(): the list is then empty (a query) or the window without rows (a fetch, read again by the next move); a
/// failed read of the platforms keeps the filters as they were. Main thread only.
class QuickCatalogModel {
public:
    static constexpr std::size_t kDefaultWindowRows = 10;
    /// Rows kept between the selection and the edge of the window it moves towards, so that the next games show.
    static constexpr std::size_t kDefaultScrollMargin = 2;

    /// Lists every game of `source`, which must outlive the model, the first one selected (once poll() collects the
    /// list, when the source answers later).
    explicit QuickCatalogModel(CatalogSource& source, std::size_t windowRows = kDefaultWindowRows,
                               std::size_t scrollMargin = kDefaultScrollMargin);
    QuickCatalogModel(const QuickCatalogModel&) = delete;
    QuickCatalogModel& operator=(const QuickCatalogModel&) = delete;

    [[nodiscard]] std::span<const PlatformFilter> filters() const noexcept { return m_filters; }
    /// The key of the active filter (empty: every game).
    [[nodiscard]] const std::string& activeFilter() const noexcept { return m_activeFilter; }
    /// Activates the filter with `key`; false, and nothing changes, for a key no filter has.
    bool setFilter(std::string_view key);
    /// Activates the filter `step` places after the active one (before it for a negative step), wrapping around the
    /// ends; false when nothing changed (a single filter, a step of 0).
    bool cycleFilter(int step);

    [[nodiscard]] const std::string& search() const noexcept { return m_search; }
    void setSearch(std::string_view text);

    /// Reads the platforms and materializes the current query again: the catalog changed (an import completed). A
    /// filter whose platform is gone falls back to every game.
    void refresh();

    /// Collects the list of a query the source runs on its worker (CatalogSource::poll) and places the selection and
    /// the window on it; true when a list (or the error of its query) arrived. Call once per frame: the screen does, in
    /// QuickCatalogScreen::afterUpdate.
    bool poll();
    /// A query runs on the source's worker: the list shown is the previous one until poll() collects the new one.
    [[nodiscard]] bool pending() const noexcept { return m_pending.has_value(); }

    /// Entries in the list.
    [[nodiscard]] std::size_t count() const noexcept { return m_count; }
    [[nodiscard]] std::size_t windowRows() const noexcept { return m_windowRows; }
    /// The rows the view shows, at least 1: the window keeps the selection's row where it can.
    void setWindowRows(std::size_t rows);
    [[nodiscard]] std::size_t scrollMargin() const noexcept;
    [[nodiscard]] std::size_t windowFirst() const noexcept { return m_first; }
    /// The rows of positions [windowFirst(), windowFirst() + windowRows()), cut at the list's end; empty while the list
    /// is, or when the source could not be read (error()).
    [[nodiscard]] std::span<const CatalogEntry> window() const noexcept { return m_window; }

    [[nodiscard]] std::optional<std::size_t> selectedPosition() const noexcept;
    /// The selection's row in the window: selectedPosition() - windowFirst().
    [[nodiscard]] std::optional<std::size_t> selectedSlot() const noexcept;
    /// The selected game, or nothing while the list is empty (or its row could not be read).
    [[nodiscard]] const CatalogEntry* selected() const noexcept;

    /// Selects the entry `id`; false, and nothing changes, when the list does not hold it.
    bool select(std::string_view id);
    /// Selects the entry at `position`: the window stays where it is when it shows the position (a click on a row),
    /// else it scrolls there. False when the list is shorter.
    bool selectPosition(std::size_t position);
    /// Moves the selection by `delta` positions, stopping at the list's ends; false when it did not move.
    bool moveSelection(std::ptrdiff_t delta);
    /// The selection and the window a page (windowRows() positions) up or down, stopping at the list's ends; false
    /// when nothing moved.
    bool pageUp();
    bool pageDown();
    /// The first and the last entry; false when the selection was there already.
    bool home();
    bool end();

    /// Incremented whenever the filters or the active filter change, also when the active one lets the same games
    /// through.
    [[nodiscard]] std::uint64_t filterRevision() const noexcept { return m_filterRevision; }
    /// Incremented whenever a list is applied: materialized, or collected by poll() (count() may stay the same).
    [[nodiscard]] std::uint64_t listRevision() const noexcept { return m_listRevision; }
    /// Incremented whenever the window's rows are read again: another list, windowFirst() or windowRows(), a read that
    /// failed before.
    [[nodiscard]] std::uint64_t windowRevision() const noexcept { return m_windowRevision; }
    /// Incremented whenever the selected game changes (another id, or none).
    [[nodiscard]] std::uint64_t selectionRevision() const noexcept { return m_selectionRevision; }

    /// The first error of the source in the latest operation that read it (a query, the platforms, the window's rows),
    /// until an operation reads it without one.
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return m_error; }

private:
    /// What a new list does when it no longer holds the selected game.
    enum class Restore : std::uint8_t {
        FirstEntry,      ///< a new search or filter: its first entry
        NearestPosition, ///< the same query again: the position the game had, or the last
    };
    /// How show() reads the window's rows.
    enum class Fetch : std::uint8_t {
        /// The rows that enter the window, when it moved (or all of them when the last read failed): a move, which
        /// starts its own error() afresh when it reads.
        Moved,
        /// Every row: a new list or window size, within an operation that started error() afresh.
        All,
    };

    /// Records the first error of the operation.
    void fail(Error error);
    /// Reads the platforms into the filters (the chip of every game first); a failed read keeps them as they were.
    void readFilters();
    /// Asks the source for the current query's list and applies it at once, or once poll() collects it.
    void materialize(Restore restore);
    /// Applies the list the source switched to (or the error of its query), placing the selection and the window.
    void applyList(const Status& listed, Restore restore);
    /// Selects `position` (kNone: nothing) with the window from `first`, reading its rows as `fetch` says.
    void show(std::size_t position, std::size_t first, Fetch fetch);
    /// Reads the window from `first`: every row, or (a scroll) the rows that enter it, moving the rows it keeps.
    void readWindow(std::size_t first, bool whole);
    /// Reads `count` rows from `position` into m_spare from `at`; false when the source failed or gave fewer rows.
    [[nodiscard]] bool readInto(std::size_t position, std::size_t count, std::size_t at);
    [[nodiscard]] std::size_t maxFirst() const noexcept;
    /// The window's first position that puts `position` at row `slot`, kept within the margins and the list.
    [[nodiscard]] std::size_t placeFirst(std::size_t position, std::size_t slot) const noexcept;
    /// The window's first position after the selection moved to `position` from the window at `first`: unchanged
    /// while it stays the margin away from the edge it moves towards (down: the bottom one), scrolled just enough
    /// otherwise.
    [[nodiscard]] std::size_t followFirst(std::size_t position, std::size_t first, bool down) const noexcept;

    CatalogSource& m_source;
    std::vector<PlatformFilter> m_filters;
    std::string m_activeFilter;
    std::string m_search;
    std::size_t m_count = 0;
    std::size_t m_windowRows = kDefaultWindowRows;
    std::size_t m_scrollMargin = kDefaultScrollMargin;
    std::size_t m_first = 0;
    /// The selected position; kNone for none.
    std::size_t m_selected;
    /// The selected game's id (to find it again in a new list).
    std::string m_selectedId;
    std::vector<CatalogEntry> m_window;
    /// The window being assembled while it scrolls, and the rows read for it (their storage reused: a scroll step
    /// allocates nothing once warm).
    std::vector<CatalogEntry> m_spare;
    std::vector<CatalogEntry> m_fetched;
    /// How the list a pending query brings places the selection (FirstEntry when a search or a filter asked since the
    /// last list); nothing while no query is pending.
    std::optional<Restore> m_pending;
    std::optional<Error> m_error;
    std::uint64_t m_filterRevision = 0;
    std::uint64_t m_listRevision = 0;
    std::uint64_t m_windowRevision = 0;
    std::uint64_t m_selectionRevision = 0;
};

} // namespace rv::ui
