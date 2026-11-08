#pragma once

// What the history plaque's card (rv_app's PlaqueCardPanel, I11 step 5; ADR-0019 §6) shares with its RmlUi document
// (assets/ui/room_plaque.rml), so that GPU-free tests check the document against them
// (tests/unit/app/test_plaque_card_document.cpp).

#include <string>
#include <string_view>

namespace rv::app {

namespace room_plaque {

inline constexpr std::string_view kDocumentPath = "ui/room_plaque.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/room_plaque.rcss";
inline constexpr std::string_view kDataModel = "room_plaque";
inline constexpr std::string_view kBodyId = "room-plaque";
inline constexpr std::string_view kNameId = "plaque-name";
inline constexpr std::string_view kDescriptionId = "plaque-description";
inline constexpr std::string_view kLogoId = "plaque-logo";
inline constexpr std::string_view kBodyRegionId = "plaque-body";

} // namespace room_plaque

/// What the card shows: the plaque's text, and the system's logo as an RML source ("rv-texture:logo-snes"), empty when
/// the UI has none.
struct PlaqueCardContent {
    std::string system;
    std::string name;
    std::string maker;
    std::string year;
    std::string type;
    std::string description;
    std::string logo;

    bool operator==(const PlaqueCardContent&) const = default;
};

} // namespace rv::app
