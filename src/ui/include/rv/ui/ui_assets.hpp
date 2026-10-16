#pragma once

// GPU-free (part of rv_ui_model, header only): the asset paths, document names and layout themes the product UI shares
// with its RmlUi documents, so that GPU-free tests can check the documents against them
// (tests/unit/ui/test_ui_documents.cpp).

#include <array>
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
/// Element id prefixes: "game-<CatalogEntry::id>", "chip-<index of the filter>".
inline constexpr std::string_view kGamePrefix = "game-";
inline constexpr std::string_view kChipPrefix = "chip-";
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
