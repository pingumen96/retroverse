#pragma once

// GPU-free (part of rv_ui_model, header only): the asset paths and document names the product UI shares with its
// RmlUi documents, so that GPU-free tests can check the documents against them (tests/unit/ui/test_ui_documents.cpp).

#include <array>
#include <string_view>

namespace rv::ui {

/// The font faces of the product UI, relative to the asset root: Atkinson Hyperlegible, a sans designed for legibility
/// (style §17.3), SIL OFL 1.1 (assets/registry). The first is the regular face. No system font is ever used.
inline constexpr std::array<std::string_view, 2> kUiFontFaces{"ui/fonts/AtkinsonHyperlegible-Regular.ttf",
                                                              "ui/fonts/AtkinsonHyperlegible-Bold.ttf"};

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
/// The body class of the high-contrast theme (specification §12.4).
inline constexpr std::string_view kHighContrastClass = "high-contrast";
} // namespace quick_catalog

} // namespace rv::ui
