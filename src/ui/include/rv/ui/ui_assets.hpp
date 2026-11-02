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
/// The body's class while the on-screen keyboard shows over the card (ADR-0018 §8).
inline constexpr std::string_view kTypingClass = "typing";
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
/// The launch readiness keys (CatalogEntry::readiness) the card labels, in ADR-0008 §8's precedence: rv_catalog's
/// LaunchReadiness names (catalog::toString), which rv_ui does not link (tests/unit/app checks that they agree).
inline constexpr std::array<std::string_view, 6> kReadinessKeys{"source-unavailable", "rom-missing",
                                                                 "requires-configuration", "emulator-not-found",
                                                                 "core-not-found", "ready"};
/// The blocker codes (CatalogEntry::blocker) the card explains in a sentence of its own: every code rv_catalog's launch
/// resolution and rv_esde's launch translation report (docs/i2/step-10.md), and the app's own (system-not-configured,
/// library-unreadable: ES-DE's configuration cannot be read). tests/unit/app/test_catalog_worker.cpp collects the codes
/// those sources report and checks that this list names each. A code the document does not name shows its readiness's
/// label alone (the blocker's own words in Informazioni and in the game-session panel).
inline constexpr std::array<std::string_view, 33> kBlockerCodes{
    "library-unreadable",
    "source-unavailable",    "rom-missing",          "rom-folder-unresolved", "rom-root-unknown",
    "system-unknown",        "system-not-configured", "no-command",           "choice-stale",
    "profile-unknown",       "profile-template",     "profile-variable-missing", "profile-invalid-request",
    "program-not-allowed",   "program-refused",      "program-repeated",     "program-missing",
    "install-unknown",       "invalid-request",      "token-unsupported",    "token-malformed",
    "command-placeholder",   "inject-file",          "start-folder-missing", "start-folder-repeated",
    "start-folder-glued",    "start-folder-relative", "find-rules-missing",  "find-rule-undecided",
    "emulator-not-found",    "core-not-found",       "core-malformed",           "core-repeated"};
/// What the list says while it shows no game of the catalog database because there is none to show yet (the
/// data model's "notice"): the catalog is being opened, there is no library.json (retroverse-cli library create), no
/// catalog database (retroverse-cli import), the user data directory lies inside the library's folders (the
/// data-directory guard), library.json names another library than the catalog's, or the catalog cannot be opened.
/// Empty: nothing to say.
inline constexpr std::string_view kNoticeOpening = "opening";
inline constexpr std::string_view kNoticeNoLibrary = "no-library";
inline constexpr std::string_view kNoticeNoDatabase = "no-database";
inline constexpr std::string_view kNoticeDataDirectory = "data-directory";
inline constexpr std::string_view kNoticeLibraryMismatch = "library-mismatch";
inline constexpr std::string_view kNoticeUnavailable = "unavailable";
inline constexpr std::array<std::string_view, 6> kNotices{kNoticeOpening,         kNoticeNoLibrary,
                                                          kNoticeNoDatabase,      kNoticeDataDirectory,
                                                          kNoticeLibraryMismatch, kNoticeUnavailable};
/// The row of filter chips (a region that scrolls with the focus), the box around it and the classes the screen sets on
/// that box while chips hide beyond the row's left or right edge (the document shows a fade there).
inline constexpr std::string_view kChipsId = "chips";
inline constexpr std::string_view kChipsBoxId = "chips-box";
inline constexpr std::string_view kMoreBeforeClass = "more-before";
inline constexpr std::string_view kMoreAfterClass = "more-after";
/// The class of the active filter's chip.
inline constexpr std::string_view kActiveChipClass = "active";
/// "Aggiorna libreria" (I3 step 13r): the button in the catalog's header (its own region, above the search field;
/// shown while the catalog can refresh) and the line under the header that tells the refresh's progress and result
/// (the data model's "refresh": its state, a phase while it runs, the summary's counts). The states and phases are
/// rv_app's RefreshOutcome and RefreshPhase names (app::toString), with "running" (tests/unit/app checks they agree).
inline constexpr std::string_view kRegionLibrary = "library";
inline constexpr std::string_view kRefreshId = "act-refresh";
inline constexpr std::string_view kRefreshStatusId = "refresh-status";
inline constexpr std::string_view kRefreshRunning = "running";
inline constexpr std::array<std::string_view, 5> kRefreshStates{kRefreshRunning, "done", "cancelled", "busy", "failed"};
inline constexpr std::array<std::string_view, 4> kRefreshPhases{"reading", "media", "comparing", "writing"};
/// The card and its title.
inline constexpr std::string_view kCardId = "card";
inline constexpr std::string_view kCardTitleId = "card-title";
/// The body class of the high-contrast theme (specification §12.4).
inline constexpr std::string_view kHighContrastClass = "high-contrast";
} // namespace quick_catalog

} // namespace rv::ui
