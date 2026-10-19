#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/ui/focus_history.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/ui_assets.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {

class UiView;

/// What the quick catalog asks of the application. Every callback is optional.
struct QuickCatalogEvents {
    /// "Avvia": A or Enter on a game, the Avvia button, a double click on a game.
    std::function<void(const CatalogEntry&)> launch;
    /// "Vedi media": Y, the button.
    std::function<void(const CatalogEntry&)> media;
    /// "Informazioni": X, the button. The card shows or hides its secondary details (`shown`).
    std::function<void(const CatalogEntry&, bool shown)> info;
    /// Back with nowhere left to go back to: the application closes the screen (or not).
    std::function<void()> close;
    /// Every move of the focus to another element (logs, reports, sounds).
    std::function<void(const FocusTarget&)> focusChanged;
    /// "Aggiorna libreria": the button (I3 step 13r). The application refreshes the catalog from ES-DE.
    std::function<void()> refresh;
    /// "Mostra nella stanza": the button, shown for a game the room places (I3 step 15a, setRoomPlacement()). The
    /// application closes the catalog and takes the walker to the game's item.
    std::function<void(const CatalogEntry&)> showInRoom;
};

/// What the quick catalog shows of the catalog's refresh (I3 step 13r): whether it offers "Aggiorna libreria", and the
/// line under the header (the document's words for each state and phase).
struct QuickCatalogRefresh {
    /// The button shows: the catalog can be refreshed now.
    bool available = false;
    /// Empty: nothing to say; else quick_catalog::kRefreshStates (running, or how the last refresh ended).
    std::string state;
    /// While it runs: quick_catalog::kRefreshPhases, with how far (`done` of `total`, 0 of 0 while unknown).
    std::string phase;
    std::size_t done = 0;
    std::size_t total = 0;
    /// Once done: games added, removed and changed, and the systems the mass-disappearance guard kept.
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t changed = 0;
    std::size_t guarded = 0;

    bool operator==(const QuickCatalogRefresh&) const = default;
};

/// The quick catalog (specification §12.2 QuickLibrary, §13.2 "Catalogo rapido"; style §16.3), the first product
/// screen: assets/ui/quick_catalog.rml bound to a QuickCatalogModel (data model "catalog": the filter chips, the rows
/// of the list's window, the selected game's card, the search text, a scroll indicator, a status line, whether the
/// catalog could be read), navigable with the pad alone.
///
///  - The list is virtualized: a pool of rows ("row-0", "row-1", ..., as many as fit the list area and at most
///    quick_catalog::kPoolRows) shows the model's window, so element ids never name a game; the screen measures how
///    many rows fit (the layout, the text scale) and gives the model that window. A game keeps its row while the
///    window holds it (quick_catalog::poolSlotOf): a scroll binds only the rows that enter and moves the rows into the
///    window's order (their style top), so a step of a held direction binds one row's text. The selected game's row
///    has the class "selected", set on the two rows concerned when the selection moves (no re-evaluation of the list).
///  - The model may change outside the screen's commands (QuickCatalogModel::refresh() after an import, a list a query
///    on the source's worker brings, which afterUpdate() collects with QuickCatalogModel::poll()): the screen shows it
///    before the next command and after every update.
///  - "Aggiorna libreria" (I3 step 13r), in the header, is a region of its own above the search field (search Up, back
///    Down), shown while setRefresh() says it is available; the line under the header tells the refresh's progress and
///    result.
///  - Regions: search field, filter chips, game list, card actions (the nav-region attribute of the document). In the
///    list Up and Down move the selection over the whole list (the model scrolls the window) and the focus follows it
///    from row to row; page moves, Home and End jump. A list that empties sends the focus to the active filter's chip
///    (from the list, or from the card's actions, hidden with no game selected). In the other regions RmlUi's spatial
///    navigation moves the focus
///    (UiView::navigate). At a region's edge region links take over (list Right -> actions, actions Left -> list, list
///    Up at the first game -> chips, chips Down -> list, chips Up -> search, search Down -> chips), landing on the
///    element the target region last focused, else on its entry: the active filter, Avvia; the list always lands on
///    the selected game (FocusRegions).
///  - The selected game's card fills (title, developer and year, the state of a game that cannot start, three lines of
///    description, platform, genre, players, developer; Informazioni adds the publisher, the year and the whole
///    description) at once after a pause and, while a held direction runs through the list, once the selection rests
///    for 0.15 s (laying the card's text out again is most of the cost of a move). A card left behind that way is
///    filled at once when the focus leaves the list or an action (Avvia, Vedi media, Informazioni) runs, so the card
///    always shows the game the actions act on.
///  - Filter chips: one row that scrolls with the focus; the active filter's chip is highlighted whenever the filter
///    changes (also when the same games pass), and fades mark the row's edges where chips hide beyond them. The
///    shoulders (FilterPrevious, FilterNext) change the filter from any region.
///  - Commands: Activate launches the selected game from the list, else presses the focused element (a chip filters, a
///    button acts, Enter in the search field moves to the list); Back returns along the FocusHistory, then asks to
///    close; Media and Info act on the selected game; Home and End in the search field move its caret.
///  - The mouse: a click focuses a row (selecting its game), a double click launches it, the wheel over the list moves
///    the selection three games per notch.
/// In developer builds the document and its style sheet reload when saved, keeping the focus. Destroy the screen before
/// its view and its model. Main thread only.
class QuickCatalogScreen {
public:
    /// Registers the data model in the view's context, loads and shows the document, sizes the model's window to the
    /// list area and focuses the selected game. `watchFiles` polls the document and the style sheet for
    /// reloadIfChanged(). Errors: the data model exists in the context already (InvalidArgument), the document cannot
    /// be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<QuickCatalogScreen>>
    create(UiView& view, QuickCatalogModel& model, QuickCatalogEvents events, bool watchFiles = false);
    ~QuickCatalogScreen();
    QuickCatalogScreen(const QuickCatalogScreen&) = delete;
    QuickCatalogScreen& operator=(const QuickCatalogScreen&) = delete;

    /// Applies the commands of a frame, in order (ignored while hidden).
    void apply(const UiCommandList& commands);
    void apply(UiCommand command);
    /// After UiView::update(): collects a list the source materialized on its worker (QuickCatalogModel::poll) and
    /// shows what the model changed, settles the card, follows the layout (the rows that fit the list area: the model's
    /// window; laid out again at once when they changed), and focuses the region's landing element again when the
    /// focused element left the document, the list or the filters when it is no longer visible.
    void afterUpdate();

    /// Shows the document again with the focus where it was; hide() keeps the focus history.
    void show();
    void hide();
    [[nodiscard]] bool visible() const noexcept;

    /// The element that has the focus.
    [[nodiscard]] std::optional<FocusTarget> focus() const;
    /// Focuses the element `id` of the document (scrolled into view); false when there is none.
    bool focusElement(std::string_view id);
    /// Where a game stands in the room, for the card (I3 step 15a, quick_catalog_room.hpp): "" while no room shows (no
    /// button, no sentence), quick_catalog::kRoomPlaced (the "Mostra nella stanza" button), or why the room does not
    /// show it (a sentence of the document). Asked whenever the card fills; the card shown fills again at once (call it
    /// again when the room changed). Unset: no room.
    void setRoomPlacement(std::function<std::string(const CatalogEntry&)> placement);
    /// The line under the card's actions ("Avvio di ..."), until another game is selected; empty hides it.
    void setStatus(std::string_view text);
    /// What the list says about the catalog itself while it shows no game of it (quick_catalog::kNotice*: the catalog
    /// being opened, no library, no catalog database, ...), in the document's words; empty says nothing.
    void setNotice(std::string_view key);
    [[nodiscard]] std::string_view notice() const noexcept;
    /// The refresh's button and line (bound only when it changed).
    void setRefresh(const QuickCatalogRefresh& refresh);
    [[nodiscard]] const QuickCatalogRefresh& refresh() const noexcept;
    /// The high-contrast theme (specification §12.4).
    void setHighContrast(bool enabled);
    [[nodiscard]] const FocusHistory& history() const noexcept;
    /// How many times the card was filled with the selected game: measurements tell the frames that laid it out again.
    [[nodiscard]] std::uint64_t cardRefreshes() const noexcept;
    /// How many times rows of the pool were bound to other games (scrolling, another list, another size).
    [[nodiscard]] std::uint64_t windowBinds() const noexcept;
    /// Rows of the pool bound to another game, in all: a scroll by one row binds one.
    [[nodiscard]] std::uint64_t rowBinds() const noexcept;
    /// The card's blocks laid out less tall than they want (their content, at most their max-height: whole lines), so
    /// that a line or a row of them is cut: each by its id, else its first class ("rows"). Empty when the card shows
    /// everything it holds whole, or is not shown. As the last layout left them (after afterUpdate()).
    [[nodiscard]] std::vector<std::string> squeezedCardBlocks() const;
    /// The count of games the screen shows (its data model's count, bound when the model's window changed): the list
    /// shown, never one still on the catalog's worker.
    [[nodiscard]] std::size_t shownCount() const noexcept;

    /// With watchFiles, reloads the document when it or its style sheet changed on disk (polled every 500 ms), keeping
    /// the focus; true when it reloaded. A document that no longer loads keeps the screen empty until fixed (logged).
    bool reloadIfChanged();

private:
    struct Impl;

    explicit QuickCatalogScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
