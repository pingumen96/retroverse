#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>

#include <rv/input/action.hpp>
#include <rv/input/input_system.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_collector.hpp>

#include "sdl_test_events.hpp"

using rv::platform::isFullscreenToggle;

namespace {

SDL_Event keyWithModifiers(SDL_Scancode scancode, SDL_Keycode key, SDL_Keymod modifiers, bool down = true,
                           bool repeat = false) {
    SDL_Event event = rv::test::keyEvent(scancode, down, repeat, key);
    event.key.mod = modifiers;
    return event;
}

} // namespace

TEST_CASE("platform/graphics_window: Alt+Enter is the fullscreen shortcut", "[platform][window]") {
    CHECK(isFullscreenToggle(keyWithModifiers(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LALT)));
    CHECK(isFullscreenToggle(keyWithModifiers(SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER, SDL_KMOD_RALT)));
    CHECK(isFullscreenToggle(
        keyWithModifiers(SDL_SCANCODE_RETURN, SDLK_RETURN, static_cast<SDL_Keymod>(SDL_KMOD_LALT | SDL_KMOD_NUM))));

    // Enter alone, key repeats, the release and Alt with another key are not.
    CHECK_FALSE(isFullscreenToggle(keyWithModifiers(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_NONE)));
    CHECK_FALSE(isFullscreenToggle(keyWithModifiers(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LALT, true, true)));
    CHECK_FALSE(isFullscreenToggle(keyWithModifiers(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LALT, false)));
    CHECK_FALSE(isFullscreenToggle(keyWithModifiers(SDL_SCANCODE_F, SDLK_F, SDL_KMOD_LALT)));
    CHECK_FALSE(isFullscreenToggle(rv::test::mouseWheelEvent(1.0f)));

    // Why the host consumes the press instead of passing it on: Enter is Confirm in the default bindings.
    rv::platform::SdlInputCollector collector;
    collector.handleEvent(keyWithModifiers(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LALT));
    rv::input::InputSystem system;
    CHECK(system.resolve(collector.endFrame()).pressed(rv::input::Action::Confirm));
}
