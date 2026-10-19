#pragma once

// GPU-free (part of rv_app_cli, header only): the asset paths, element ids and data model that the hover's title label
// (rv_app's WorldLabelPanel, I3 step 13a; ADR-0012 §3) shares with its RmlUi document (assets/ui/world_label.rml), so
// that GPU-free tests check the document against them (tests/unit/app/test_world_label_document.cpp). The words around
// the title are the document's: the code passes the catalog's title and a position.

#include <string>
#include <string_view>

namespace rv::app {

namespace world_label {

inline constexpr std::string_view kDocumentPath = "ui/world_label.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/world_label.rcss";
/// The data model: "title" (the catalog's title; "" for a game the snapshot lacks), "available" (false: its file is not
/// there), "left", "bottom", "width" (the label's band, CSS lengths in pixels of the view: the title is centred in it).
inline constexpr std::string_view kDataModel = "world_label";
inline constexpr std::string_view kLabelId = "world-label";
/// The label stands this many pixels above the item's projected top edge, and never closer than kTopMargin pixels to
/// the view's top.
inline constexpr float kGapPixels = 12.0f;
inline constexpr float kTopMargin = 64.0f;

} // namespace world_label

/// What the hover's label shows and where (world_label_ui.hpp).
struct WorldLabelContent {
    std::string title;
    bool available = true;
    /// The point the label stands on (its bottom's middle), in pixels of the view (y down).
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const WorldLabelContent&) const = default;
};

} // namespace rv::app
