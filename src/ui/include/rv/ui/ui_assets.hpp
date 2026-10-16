#pragma once

// GPU-free (part of rv_ui_model, header only): the asset paths, document names and layout themes the product UI shares
// with its RmlUi documents, so that GPU-free tests can check the documents against them
// (tests/unit/ui/test_ui_documents.cpp).

#include <array>
#include <cstddef>
#include <string_view>

namespace rv::ui {

/// The font faces of the product UI, relative to the asset root: Atkinson Hyperlegible, a sans designed for legibility
/// (style §17.3), SIL OFL 1.1 (assets/registry). The first is the regular face. No system font is ever used.
inline constexpr std::array<std::string_view, 2> kUiFontFaces{"ui/fonts/AtkinsonHyperlegible-Regular.ttf",
                                                              "ui/fonts/AtkinsonHyperlegible-Bold.ttf"};

/// The RmlUi theme of small views, which documents style with "@media (theme: compact)": a denser layout so that
/// everything stays on screen. UiView activates it while compactLayout() holds.
inline constexpr std::string_view kCompactTheme = "compact";
/// Views less tall than this, in dp of the display alone, use the compact theme: 1280x720 at 125% (576), 1920x1080 at
/// 150% (720), but not 1920x1080 at 125% (864).
inline constexpr float kCompactBelowDisplayDp = 760.0f;

/// True when a drawable `drawableHeight` pixels tall on a display of content scale `displayScale` uses the compact
/// theme. The text scale of the accessibility settings (specification §12.4) is left out on purpose: it enlarges the
/// text of either layout instead of switching to the smaller sizes of the compact one, which would cancel it. A scale
/// that is not positive counts as 1.
[[nodiscard]] constexpr bool compactLayout(int drawableHeight, float displayScale) noexcept {
    const float scale = displayScale > 0.0f ? displayScale : 1.0f;
    return static_cast<float>(drawableHeight) < kCompactBelowDisplayDp * scale;
}

/// Element ids, regions and the data model name the quick catalog document (assets/ui/quick_catalog.rml) and its screen
/// (rv::ui::QuickCatalogScreen) share.
namespace quick_catalog {
inline constexpr std::string_view kDocumentPath = "ui/quick_catalog.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/quick_catalog.rcss";
inline constexpr std::string_view kDataModel = "catalog";
/// The attribute that names the region of the elements inside an element.
inline constexpr std::string_view kRegionAttribute = "nav-region";
inline constexpr std::string_view kRegionSearch = "search";
inline constexpr std::string_view kRegionChips = "chips";
inline constexpr std::string_view kRegionList = "list";
inline constexpr std::string_view kRegionActions = "actions";
inline constexpr std::string_view kSearchId = "search";
inline constexpr std::string_view kPlayId = "act-play";
inline constexpr std::string_view kMediaId = "act-media";
inline constexpr std::string_view kInfoId = "act-info";
/// Element id prefixes: "row-<slot of the pool>", "chip-<index of the filter>".
inline constexpr std::string_view kRowPrefix = "row-";
inline constexpr std::string_view kChipPrefix = "chip-";
/// The list is virtualized: a pool of rows (the data model's "rows", elements "row-0", "row-1", ...), as many as fit
/// the list area and at most kPoolRows, shows the window of the list. The largest supported layouts fit 24 rows (a
/// 2160-pixel-tall view at 100%, text scale 1; a 1080-pixel one at 125% fits 7, the compact 1280x720 one at 125% 5):
/// the pool holds them and a margin. Taller views (text scale below 1) show kPoolRows rows.
inline constexpr std::size_t kPoolRows = 28;

/// The row of the pool (of `poolRows` rows) that shows the list's position `position` while the window holds it: a game
/// keeps its row as the window scrolls, so a scroll by one re-binds the one row that enters, and the screen moves the
/// rows into the window's order (QuickCatalogScreen). 0 for an empty pool.
[[nodiscard]] constexpr std::size_t poolSlotOf(std::size_t position, std::size_t poolRows) noexcept {
    return poolRows > 0 ? position % poolRows : 0;
}
/// The game list (the region whose rows the pool binds), the class the screen sets on the selected game's row, and
/// the scroll indicator beside the list (its thumb shows the window's place in the list).
inline constexpr std::string_view kListId = "game-list";
inline constexpr std::string_view kSelectedClass = "selected";
inline constexpr std::string_view kScrollThumbId = "list-thumb";
/// The state keys (CatalogEntry::state) the document labels on the card and styles in the list: the game's file
/// cannot be reached (a missing file, a source unavailable), its emulator needs configuring (a command Retroverse does
/// not run as it is), its emulator or core is not found (ADR-0008 §4, §8).
inline constexpr std::string_view kStateUnavailable = "unavailable";
inline constexpr std::string_view kStateNeedsConfiguration = "needs-configuration";
inline constexpr std::string_view kStateEmulatorMissing = "emulator-missing";
/// The row of filter chips (a region that scrolls with the focus), the box around it and the classes the screen sets on
/// that box while chips hide beyond the row's left or right edge (the document shows a fade there).
inline constexpr std::string_view kChipsId = "chips";
inline constexpr std::string_view kChipsBoxId = "chips-box";
inline constexpr std::string_view kMoreBeforeClass = "more-before";
inline constexpr std::string_view kMoreAfterClass = "more-after";
/// The class of the active filter's chip.
inline constexpr std::string_view kActiveChipClass = "active";
/// The card's title.
inline constexpr std::string_view kCardTitleId = "card-title";
/// The body class of the high-contrast theme (specification §12.4).
inline constexpr std::string_view kHighContrastClass = "high-contrast";
} // namespace quick_catalog

} // namespace rv::ui
