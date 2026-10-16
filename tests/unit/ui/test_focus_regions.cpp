#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <rv/ui/focus_regions.hpp>

using rv::ui::FocusRegions;
using rv::ui::FocusTarget;
using rv::ui::NavDirection;

TEST_CASE("ui/focus_regions: links lead to the entry element until the region remembers one", "[ui][focus_regions]") {
    FocusRegions regions;
    regions.link("list", NavDirection::Right, "actions");
    regions.link("actions", NavDirection::Left, "list");
    regions.setEntry("actions", "act-play");
    regions.setEntry("list", "game-g01");

    CHECK(regions.linked("list", NavDirection::Right) == "actions");
    CHECK_FALSE(regions.linked("list", NavDirection::Left).has_value());
    CHECK_FALSE(regions.linked("nowhere", NavDirection::Up).has_value());

    std::optional<FocusTarget> next = regions.across("list", NavDirection::Right);
    REQUIRE(next.has_value());
    CHECK(*next == FocusTarget{"actions", "act-play"});

    // The focus was on the "Informazioni" button: coming back from the list lands there.
    regions.onFocused(FocusTarget{"actions", "act-info"});
    regions.onFocused(FocusTarget{"list", "game-g11"});
    CHECK(regions.across("list", NavDirection::Right) == FocusTarget{"actions", "act-info"});
    CHECK(regions.across("actions", NavDirection::Left) == FocusTarget{"list", "game-g11"});
    CHECK(regions.remembered("list") == "game-g11");
    CHECK(regions.entry("list") == "game-g01");
}

TEST_CASE("ui/focus_regions: a forgotten element falls back to the entry", "[ui][focus_regions]") {
    FocusRegions regions;
    regions.link("chips", NavDirection::Down, "list");
    regions.onFocused(FocusTarget{"list", "game-g05"});
    CHECK(regions.landing("list") == "game-g05");
    regions.forget("list"); // the game was filtered out
    CHECK_FALSE(regions.landing("list").has_value());
    CHECK_FALSE(regions.across("chips", NavDirection::Down).has_value()); // no element to land on
    regions.setEntry("list", "game-g02");
    CHECK(regions.across("chips", NavDirection::Down) == FocusTarget{"list", "game-g02"});
    regions.forget("unknown"); // harmless
}

TEST_CASE("ui/focus_regions: a later link replaces an earlier one", "[ui][focus_regions]") {
    FocusRegions regions;
    regions.link("search", NavDirection::Down, "chips");
    regions.link("search", NavDirection::Down, "list");
    CHECK(regions.linked("search", NavDirection::Down) == "list");
    CHECK_FALSE(regions.landing("list").has_value());
}
