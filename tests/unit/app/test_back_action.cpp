#include <catch2/catch_test_macros.hpp>

#include <rv/app/back_action.hpp>

using rv::app::BackAction;
using rv::app::backAction;

TEST_CASE("app/back_action: Back leaves one level at a time", "[app][back_action]") {
    // Without the overlay (and in builds without it): release a captured mouse, then quit.
    CHECK(backAction(true, false, false) == BackAction::ReleaseMouse);
    CHECK(backAction(false, false, false) == BackAction::Quit);

    // Walking with the overlay open: the mouse first, then the overlay, never straight to quitting.
    CHECK(backAction(true, true, false) == BackAction::ReleaseMouse);
    CHECK(backAction(false, true, false) == BackAction::CloseOverlay);

    // A text field of the overlay has the keyboard: Esc only leaves the field (the pad's B too).
    CHECK(backAction(false, true, true) == BackAction::Ignore);
    CHECK(backAction(true, true, true) == BackAction::Ignore);
    // A hidden overlay never has the keyboard.
    CHECK(backAction(false, false, true) == BackAction::Quit);
}
