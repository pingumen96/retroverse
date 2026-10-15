#include <catch2/catch_test_macros.hpp>

#include <array>

#include <rv/devui/overlay_input.hpp>

using rv::devui::CaptureHandover;
using rv::devui::EventRoute;
using rv::devui::InputEventKind;
using rv::devui::OverlayInputState;
using rv::devui::routeOverlayEvent;

namespace {

constexpr std::array kAllKinds{InputEventKind::MouseMotion,   InputEventKind::MouseButtonDown,
                               InputEventKind::MouseButtonUp, InputEventKind::MouseWheel,
                               InputEventKind::KeyDown,       InputEventKind::KeyUp,
                               InputEventKind::TextInput,     InputEventKind::Other};

constexpr EventRoute kGameOnly{false, true};
constexpr EventRoute kOverlayOnly{true, false};
constexpr EventRoute kBoth{true, true};

OverlayInputState visible(bool wantsMouse, bool wantsKeyboard, bool mouseCaptured) {
    return OverlayInputState{true, wantsMouse, wantsKeyboard, mouseCaptured};
}

} // namespace

TEST_CASE("devui/overlay_input: a hidden overlay leaves every event to the game", "[devui][overlay_input]") {
    for (const InputEventKind kind : kAllKinds) {
        // Whatever Dear ImGui reported before it was hidden.
        CHECK(routeOverlayEvent(kind, OverlayInputState{false, true, true, false}) == kGameOnly);
        CHECK(routeOverlayEvent(kind, OverlayInputState{false, false, false, true}) == kGameOnly);
    }
}

TEST_CASE("devui/overlay_input: the mouse goes to the panels unless the game looks around with it",
          "[devui][overlay_input]") {
    // Over a panel: the overlay only, except releases, which the game gets too.
    const OverlayInputState overPanel = visible(true, false, false);
    CHECK(routeOverlayEvent(InputEventKind::MouseMotion, overPanel) == kOverlayOnly);
    CHECK(routeOverlayEvent(InputEventKind::MouseButtonDown, overPanel) == kOverlayOnly);
    CHECK(routeOverlayEvent(InputEventKind::MouseWheel, overPanel) == kOverlayOnly);
    CHECK(routeOverlayEvent(InputEventKind::MouseButtonUp, overPanel) == kBoth);

    // Over the scene: both (the click that captures the mouse also ends a text edit in a panel).
    const OverlayInputState overScene = visible(false, false, false);
    CHECK(routeOverlayEvent(InputEventKind::MouseMotion, overScene) == kBoth);
    CHECK(routeOverlayEvent(InputEventKind::MouseButtonDown, overScene) == kBoth);
    CHECK(routeOverlayEvent(InputEventKind::MouseButtonUp, overScene) == kBoth);
    CHECK(routeOverlayEvent(InputEventKind::MouseWheel, overScene) == kBoth);

    // Captured for first-person look: the game only, whatever the overlay reported.
    for (const bool wantsMouse : {false, true}) {
        const OverlayInputState looking = visible(wantsMouse, false, true);
        CHECK(routeOverlayEvent(InputEventKind::MouseMotion, looking) == kGameOnly);
        CHECK(routeOverlayEvent(InputEventKind::MouseButtonDown, looking) == kGameOnly);
        CHECK(routeOverlayEvent(InputEventKind::MouseButtonUp, looking) == kGameOnly);
        CHECK(routeOverlayEvent(InputEventKind::MouseWheel, looking) == kGameOnly);
    }
}

TEST_CASE("devui/overlay_input: keys reach the game unless a panel has the keyboard", "[devui][overlay_input]") {
    // Walking with the overlay open: keys reach both.
    const OverlayInputState walking = visible(false, false, false);
    CHECK(routeOverlayEvent(InputEventKind::KeyDown, walking) == kBoth);
    CHECK(routeOverlayEvent(InputEventKind::KeyUp, walking) == kBoth);

    // A text field has the keyboard: presses (Esc included) stay in the overlay, releases reach the game as well, so a
    // key held before the field took the keyboard does not stay held in the game.
    for (const bool mouseCaptured : {false, true}) {
        const OverlayInputState typing = visible(true, true, mouseCaptured);
        CHECK(routeOverlayEvent(InputEventKind::KeyDown, typing) == kOverlayOnly);
        CHECK(routeOverlayEvent(InputEventKind::KeyUp, typing) == kBoth);
        CHECK(routeOverlayEvent(InputEventKind::TextInput, typing) == kOverlayOnly);
    }
    // Text input never reaches the game; window, gamepad and quit events reach both.
    CHECK(routeOverlayEvent(InputEventKind::TextInput, walking) == kOverlayOnly);
    CHECK(routeOverlayEvent(InputEventKind::Other, walking) == kBoth);
    CHECK(routeOverlayEvent(InputEventKind::Other, visible(true, true, true)) == kBoth);
}

TEST_CASE("devui/overlay_input: closing the overlay gives back the capture it took", "[devui][overlay_input]") {
    CaptureHandover handover;
    // Walking with the mouse captured: opening releases it, closing captures it again.
    CHECK(handover.open(true));
    CHECK(handover.close(false));

    // Opened with a free mouse: nothing to release or to restore.
    CHECK_FALSE(handover.open(false));
    CHECK_FALSE(handover.close(false));

    // A capture taken while the overlay is open (a click into the scene) is kept; nothing more to capture.
    CHECK_FALSE(handover.open(false));
    CHECK_FALSE(handover.close(true));
    CHECK(handover.open(true));
    CHECK_FALSE(handover.close(true)); // captured again in the meantime

    // Closing twice restores at most once.
    CHECK(handover.open(true));
    CHECK(handover.close(false));
    CHECK_FALSE(handover.close(false));
}
