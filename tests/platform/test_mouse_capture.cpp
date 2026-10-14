#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <rv/platform/mouse_capture.hpp>

#include "sdl_test_events.hpp"

using rv::platform::MouseCapture;
using rv::test::mouseButtonEvent;
using rv::test::windowEvent;

// A hidden window never receives focus, so focus changes are synthetic events; capture itself is SDL's real relative
// mouse mode on that window (SDL_GetWindowRelativeMouseMode reports it).

TEST_CASE("platform/mouse_capture: a click into the focused window captures, focus loss releases",
          "[platform][mouse_capture]") {
    rv::test::HiddenWindowScope sdl;
    if (sdl.window() == nullptr) {
        SKIP("SDL video unavailable: " << SDL_GetError());
    }
    const SDL_WindowID id = sdl.id();
    MouseCapture capture(sdl.window());
    CHECK_FALSE(capture.captured());
    CHECK_FALSE(capture.focused());

    // Without focus a click does nothing.
    CHECK_FALSE(capture.handleEvent(mouseButtonEvent(SDL_BUTTON_LEFT, true, id)));
    CHECK_FALSE(capture.captured());

    // Focused: the click captures and is consumed; later clicks reach the scene.
    CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id)));
    CHECK(capture.focused());
    CHECK_FALSE(capture.captured()); // focus alone does not capture
    CHECK(capture.handleEvent(mouseButtonEvent(SDL_BUTTON_LEFT, true, id)));
    CHECK(capture.captured());
    CHECK(SDL_GetWindowRelativeMouseMode(sdl.window()));
    CHECK_FALSE(capture.handleEvent(mouseButtonEvent(SDL_BUTTON_LEFT, true, id)));
    CHECK_FALSE(capture.handleEvent(mouseButtonEvent(SDL_BUTTON_LEFT, false, id)));

    // Events of other windows are ignored.
    CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, id + 1)));
    CHECK(capture.captured());

    // Focus loss releases; regaining focus needs a new click.
    CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, id)));
    CHECK_FALSE(capture.captured());
    CHECK_FALSE(SDL_GetWindowRelativeMouseMode(sdl.window()));
    CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id)));
    CHECK_FALSE(capture.captured());
    CHECK(capture.handleEvent(mouseButtonEvent(SDL_BUTTON_RIGHT, true, id)));
    CHECK(capture.captured());

    // Explicit release (Back) and capture.
    CHECK_FALSE(capture.setCaptured(false, "Back pressed"));
    CHECK_FALSE(SDL_GetWindowRelativeMouseMode(sdl.window()));
    CHECK(capture.setCaptured(true));
    CHECK(capture.setCaptured(true)); // idempotent
    CHECK(SDL_GetWindowRelativeMouseMode(sdl.window()));
}

TEST_CASE("platform/mouse_capture: startup capture waits for the focus", "[platform][mouse_capture]") {
    rv::test::HiddenWindowScope sdl;
    if (sdl.window() == nullptr) {
        SKIP("SDL video unavailable: " << SDL_GetError());
    }
    const SDL_WindowID id = sdl.id();
    {
        MouseCapture capture(sdl.window());
        capture.captureWhenFocused();
        CHECK_FALSE(capture.captured());
        CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id)));
        CHECK(capture.captured());
        // Only once: after a release, the next focus gain does not capture again.
        CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, id)));
        CHECK_FALSE(capture.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id)));
        CHECK_FALSE(capture.captured());
        CHECK(capture.setCaptured(true));
    }
    // Destruction released the captured mouse.
    CHECK_FALSE(SDL_GetWindowRelativeMouseMode(sdl.window()));

    // An explicit release cancels a pending startup capture.
    MouseCapture cancelled(sdl.window());
    cancelled.captureWhenFocused();
    CHECK_FALSE(cancelled.setCaptured(false));
    CHECK_FALSE(cancelled.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id)));
    CHECK_FALSE(cancelled.captured());

    // A focused window captures at once.
    MouseCapture immediate(sdl.window());
    CHECK_FALSE(immediate.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id)));
    immediate.captureWhenFocused();
    CHECK(immediate.captured());
}
