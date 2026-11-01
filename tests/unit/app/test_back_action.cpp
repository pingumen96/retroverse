#include <catch2/catch_test_macros.hpp>

#include <rv/app/back_action.hpp>
#include <rv/app/world_interaction.hpp>

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

TEST_CASE("app/back_action: the directory is a level of the product UI's, under its game-session panel",
          "[app][back_action]") {
    // I4 step 7b: Esc or B leaves the directory one level at a time (the card, the rooms, then it closes).
    CHECK(backAction(BackState{.directoryOpen = true}) == BackAction::DirectoryBack);
    CHECK(backAction(BackState{.mouseCaptured = true, .overlayVisible = true, .directoryOpen = true}) ==
          BackAction::DirectoryBack);
    // A launch from its card: the game-session panel over it first.
    CHECK(backAction(BackState{.panelOpen = true, .directoryOpen = true}) == BackAction::PanelBack);
    // The overlay's field and popup stay the innermost levels.
    CHECK(backAction(BackState{.overlayVisible = true, .overlayHasKeyboard = true, .directoryOpen = true}) ==
          BackAction::Ignore);
    CHECK(rv::app::toString(BackAction::DirectoryBack) == "directory back");
    // In the world, the shell's level: the world's own levels (Focus, Inspect) wait (the directory opens in free roam).
    const rv::app::WorldBackDecision decision =
        rv::app::worldBackAction(BackState{.directoryOpen = true}, rv::app::WorldBack::LeaveFocus);
    CHECK(decision.level == rv::app::WorldBack::Shell);
    CHECK(decision.shell == BackAction::DirectoryBack);
}

TEST_CASE("app/back_action: where a product UI exists Back's last level is the menu, never quitting",
          "[app][back_action]") {
    // ADR-0018 §1: with a pad, B in free roam used to end the program at once.
    CHECK(backAction(BackState{.productUi = true}) == BackAction::OpenMenu);
    // Runs without a product UI (the model viewer, the asset preview) still quit.
    CHECK(backAction(BackState{.productUi = false}) == BackAction::Quit);
    // The keyboard reaches it with Esc once the mouse is free, through the overlay when it shows.
    CHECK(backAction(BackState{.mouseCaptured = true, .productUi = true}) == BackAction::ReleaseMouse);
    CHECK(backAction(BackState{.overlayVisible = true, .productUi = true}) == BackAction::CloseOverlay);
    // Every other level comes first.
    CHECK(backAction(BackState{.overlayVisible = true, .overlayHasKeyboard = true, .productUi = true}) ==
          BackAction::Ignore);
    CHECK(backAction(BackState{.panelOpen = true, .productUi = true}) == BackAction::PanelBack);
    CHECK(backAction(BackState{.catalogOpen = true, .productUi = true}) == BackAction::CatalogBack);
    CHECK(backAction(BackState{.directoryOpen = true, .productUi = true}) == BackAction::DirectoryBack);
}

TEST_CASE("app/back_action: the menu is a level of the product UI's, under its game-session panel", "[app][back_action]") {
    CHECK(backAction(BackState{.menuOpen = true, .productUi = true}) == BackAction::MenuBack);
    // Open, the mouse and the overlay wait for it, as for the directory.
    CHECK(backAction(BackState{.mouseCaptured = true, .overlayVisible = true, .menuOpen = true, .productUi = true}) ==
          BackAction::MenuBack);
    CHECK(backAction(BackState{.panelOpen = true, .menuOpen = true, .productUi = true}) == BackAction::PanelBack);
    CHECK(backAction(BackState{.overlayVisible = true, .overlayPopupOpen = true, .menuOpen = true, .productUi = true}) ==
          BackAction::ClosePopup);
    CHECK(rv::app::toString(BackAction::MenuBack) == "menu back");
    CHECK(rv::app::toString(BackAction::OpenMenu) == "open menu");
}

TEST_CASE("app/back_action: the world's own levels come before the menu opens", "[app][back_action]") {
    using rv::app::WorldBack;
    // Focus and Inspect are left first; only free roam reaches the menu.
    const rv::app::WorldBackDecision focus = rv::app::worldBackAction(BackState{.productUi = true}, WorldBack::LeaveFocus);
    CHECK(focus.level == WorldBack::LeaveFocus);
    const rv::app::WorldBackDecision roam = rv::app::worldBackAction(BackState{.productUi = true}, WorldBack::Shell);
    CHECK(roam.level == WorldBack::Shell);
    CHECK(roam.shell == BackAction::OpenMenu);
    // The menu open: its level, whatever the world's.
    const rv::app::WorldBackDecision open =
        rv::app::worldBackAction(BackState{.menuOpen = true, .productUi = true}, WorldBack::LeaveFocus);
    CHECK(open.level == WorldBack::Shell);
    CHECK(open.shell == BackAction::MenuBack);
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
