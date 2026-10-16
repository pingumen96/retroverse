#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

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
};

/// The quick catalog (specification §12.2 QuickLibrary, §13.2 "Catalogo rapido"; style §16.3), the first product
/// screen: assets/ui/quick_catalog.rml bound to a QuickCatalogModel (data model "catalog": the filter chips, the
/// visible games, the selected game's card, the search text, a status line), navigable with the pad alone.
///
///  - Regions: search field, filter chips, game list, card actions (the nav-region attribute of the document). Inside a
///    region RmlUi's spatial navigation moves the focus (UiView::navigate); at its edge region links take over (list
///    Right -> actions, actions Left -> list, list Up -> chips, chips Down -> list, chips Up -> search, search Down ->
///    chips), landing on the element the target region last focused, else on its entry: the selected game, the active
///    filter, Avvia (FocusRegions).
///  - The focused game is the selected one: its card fills (title, developer and year, three lines of description,
///    platform, genre, players, developer; Informazioni adds the publisher, the year and the whole description), at
///    once after a pause and, while a held direction runs through the list, once the focus rests for 0.15 s (laying
///    the card's text out again is most of the cost of a move). A card left behind that way is filled at once when the
///    focus leaves the list or an action (Avvia, Vedi media, Informazioni) runs, so the card always shows the game the
///    actions act on. The list scrolls with the focus.
///  - Filter chips: one row that scrolls with the focus; the active filter's chip is highlighted whenever the filter
///    changes (also when the same games pass), and fades mark the row's edges where chips hide beyond them.
///  - Commands: Activate launches the focused game, else presses the focused element (a chip filters, a button acts,
///    Enter in the search field moves to the list); Back returns along the FocusHistory, then asks to close; Media and
///    Info act on the selected game.
/// In developer builds the document and its style sheet reload when saved, keeping the focus. Destroy the screen before
/// its view and its model. Main thread only.
class QuickCatalogScreen {
public:
    /// Registers the data model in the view's context, loads and shows the document and focuses the selected game.
    /// `watchFiles` polls the document and the style sheet for reloadIfChanged(). Errors: the data model exists in the
    /// context already (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<QuickCatalogScreen>>
    create(UiView& view, QuickCatalogModel& model, QuickCatalogEvents events, bool watchFiles = false);
    ~QuickCatalogScreen();
    QuickCatalogScreen(const QuickCatalogScreen&) = delete;
    QuickCatalogScreen& operator=(const QuickCatalogScreen&) = delete;

    /// Applies the commands of a frame, in order (ignored while hidden).
    void apply(const UiCommandList& commands);
    void apply(UiCommand command);
    /// After UiView::update(): when the focused element left the document (its game was filtered out), focuses the
    /// region's landing element again.
    void afterUpdate();

    /// Shows the document again with the focus where it was; hide() keeps the focus history.
    void show();
    void hide();
    [[nodiscard]] bool visible() const noexcept;

    /// The element that has the focus.
    [[nodiscard]] std::optional<FocusTarget> focus() const;
    /// Focuses the element `id` of the document (scrolled into view); false when there is none.
    bool focusElement(std::string_view id);
    /// The line under the card's actions ("Avvio di ..."), until another game is selected; empty hides it.
    void setStatus(std::string_view text);
    /// The high-contrast theme (specification §12.4).
    void setHighContrast(bool enabled);
    [[nodiscard]] const FocusHistory& history() const noexcept;
    /// How many times the card was filled with the selected game: measurements tell the frames that laid it out again.
    [[nodiscard]] std::uint64_t cardRefreshes() const noexcept;

    /// With watchFiles, reloads the document when it or its style sheet changed on disk (polled every 500 ms), keeping
    /// the focus; true when it reloaded. A document that no longer loads keeps the screen empty until fixed (logged).
    bool reloadIfChanged();

private:
    struct Impl;

    explicit QuickCatalogScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
