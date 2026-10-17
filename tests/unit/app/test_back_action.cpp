#include <catch2/catch_test_macros.hpp>

#include <rv/app/back_action.hpp>

using rv::app::BackAction;
using rv::app::backAction;
using rv::app::BackState;

TEST_CASE("app/back_action: Back leaves one level at a time", "[app][back_action]") {
    // Without the overlay (and in builds without it): release a captured mouse, then quit.
    CHECK(backAction(BackState{.mouseCaptured = true}) == BackAction::ReleaseMouse);
    CHECK(backAction(BackState{}) == BackAction::Quit);

    // Walking with the overlay open: the mouse first, then the overlay, never straight to quitting.
    CHECK(backAction(BackState{.mouseCaptured = true, .overlayVisible = true}) == BackAction::ReleaseMouse);
    CHECK(backAction(BackState{.overlayVisible = true}) == BackAction::CloseOverlay);

    // A text field of the overlay has the keyboard: Esc only leaves the field (the pad's B too).
    CHECK(backAction(BackState{.overlayVisible = true, .overlayHasKeyboard = true}) == BackAction::Ignore);
    CHECK(backAction(BackState{.mouseCaptured = true, .overlayVisible = true, .overlayHasKeyboard = true}) ==
          BackAction::Ignore);
    // A hidden overlay never has the keyboard.
    CHECK(backAction(BackState{.overlayHasKeyboard = true}) == BackAction::Quit);
}

TEST_CASE("app/back_action: an open popup of the overlay is the innermost level", "[app][back_action]") {
    // A combo's list or a color picker is open: Back closes it and leaves the overlay open.
    CHECK(backAction(BackState{.overlayVisible = true, .overlayPopupOpen = true}) == BackAction::ClosePopup);
    CHECK(backAction(BackState{.mouseCaptured = true, .overlayVisible = true, .overlayPopupOpen = true}) ==
          BackAction::ClosePopup);
    // A field being edited in the popup comes first: Esc leaves the field, the next Back closes the popup.
    CHECK(backAction(BackState{.overlayVisible = true, .overlayHasKeyboard = true, .overlayPopupOpen = true}) ==
          BackAction::Ignore);
    // A hidden overlay has no popups.
    CHECK(backAction(BackState{.overlayPopupOpen = true}) == BackAction::Quit);
}

TEST_CASE("app/back_action: the product UI's levels come before the mouse, the overlay and quitting",
          "[app][back_action]") {
    // The quick catalog open: Back walks through it (its focus history, then it closes), never straight to quitting.
    CHECK(backAction(BackState{.catalogOpen = true}) == BackAction::CatalogBack);
    // The game-session panel over it (a game asking, an outcome): the panel first, a popup of the product UI.
    CHECK(backAction(BackState{.panelOpen = true, .catalogOpen = true}) == BackAction::PanelBack);
    CHECK(backAction(BackState{.panelOpen = true}) == BackAction::PanelBack);
    // The developer overlay's field and popup stay the innermost levels: Esc in a field never leaves the catalog.
    CHECK(backAction(BackState{.overlayVisible = true, .overlayHasKeyboard = true, .panelOpen = true,
                               .catalogOpen = true}) == BackAction::Ignore);
    CHECK(backAction(BackState{.overlayVisible = true, .overlayPopupOpen = true, .catalogOpen = true}) ==
          BackAction::ClosePopup);
    // The overlay open beside the catalog: the catalog is left first, then the overlay closes, then Back quits.
    CHECK(backAction(BackState{.overlayVisible = true, .catalogOpen = true}) == BackAction::CatalogBack);
    CHECK(backAction(BackState{.overlayVisible = true}) == BackAction::CloseOverlay);
    // A captured mouse with the catalog open (it never is: opening the catalog frees it) still leaves the catalog.
    CHECK(backAction(BackState{.mouseCaptured = true, .catalogOpen = true}) == BackAction::CatalogBack);
}

TEST_CASE("app/back_action: every level has a name for the log", "[app][back_action]") {
    CHECK(rv::app::toString(BackAction::Ignore) == "ignore");
    CHECK(rv::app::toString(BackAction::ClosePopup) == "close popup");
    CHECK(rv::app::toString(BackAction::PanelBack) == "panel back");
    CHECK(rv::app::toString(BackAction::CatalogBack) == "catalog back");
    CHECK(rv::app::toString(BackAction::ReleaseMouse) == "release mouse");
    CHECK(rv::app::toString(BackAction::CloseOverlay) == "close overlay");
    CHECK(rv::app::toString(BackAction::Quit) == "quit");
}
