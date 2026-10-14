#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <rv/input/action.hpp>
#include <rv/input/codes.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/platform/sdl_input_collector.hpp>

#include "sdl_test_events.hpp"

using rv::input::DeviceKind;
using rv::input::Key;
using rv::input::MouseButton;
using rv::input::toIndex;
using rv::platform::SdlInputCollector;
using rv::test::keyEvent;
using rv::test::mouseButtonEvent;

// These tests feed synthetic events: the collector itself never calls SDL for keyboard and mouse input, so SDL is not
// initialized here (gamepads, which SDL must open, are covered by test_gamepad_manager.cpp).

TEST_CASE("platform/sdl_input_collector: keys by scancode with per-frame edges", "[platform][collector]") {
    SdlInputCollector collector;
    // AZERTY: the physical W position prints Z; the scancode, not the key code, decides.
    collector.handleEvent(keyEvent(SDL_SCANCODE_W, true, false, SDLK_Z));
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.keysDown[toIndex(Key::W)]);
    CHECK(frame.keysPressed[toIndex(Key::W)]);
    CHECK_FALSE(frame.keysDown[toIndex(Key::Z)]);

    // Held keys persist, edges do not; key repeats are ignored.
    collector.handleEvent(keyEvent(SDL_SCANCODE_W, true, true));
    frame = collector.endFrame();
    CHECK(frame.keysDown[toIndex(Key::W)]);
    CHECK_FALSE(frame.keysPressed[toIndex(Key::W)]);

    collector.handleEvent(keyEvent(SDL_SCANCODE_W, false));
    frame = collector.endFrame();
    CHECK_FALSE(frame.keysDown[toIndex(Key::W)]);
    CHECK(frame.keysReleased[toIndex(Key::W)]);
    frame = collector.endFrame();
    CHECK_FALSE(frame.keysReleased[toIndex(Key::W)]);

    // A tap within one frame reports both edges, not held.
    collector.handleEvent(keyEvent(SDL_SCANCODE_F12, true));
    collector.handleEvent(keyEvent(SDL_SCANCODE_F12, false));
    frame = collector.endFrame();
    CHECK(frame.keysPressed[toIndex(Key::F12)]);
    CHECK(frame.keysReleased[toIndex(Key::F12)]);
    CHECK_FALSE(frame.keysDown[toIndex(Key::F12)]);

    // A release without a press (key held before the window had focus) and unmapped keys are ignored.
    collector.handleEvent(keyEvent(SDL_SCANCODE_A, false));
    collector.handleEvent(keyEvent(SDL_SCANCODE_CAPSLOCK, true));
    frame = collector.endFrame();
    CHECK(frame.keysReleased.none());
    CHECK(frame.keysDown.none());
}

TEST_CASE("platform/sdl_input_collector: two physical keys of one code stay held until both are up",
          "[platform][collector]") {
    SdlInputCollector collector;
    collector.handleEvent(keyEvent(SDL_SCANCODE_RETURN, true));
    collector.handleEvent(keyEvent(SDL_SCANCODE_KP_ENTER, true));
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.keysDown[toIndex(Key::Enter)]);
    CHECK(frame.keysPressed[toIndex(Key::Enter)]);

    collector.handleEvent(keyEvent(SDL_SCANCODE_RETURN, false));
    frame = collector.endFrame();
    CHECK(frame.keysDown[toIndex(Key::Enter)]);
    CHECK_FALSE(frame.keysReleased[toIndex(Key::Enter)]);

    collector.handleEvent(keyEvent(SDL_SCANCODE_KP_ENTER, false));
    frame = collector.endFrame();
    CHECK_FALSE(frame.keysDown[toIndex(Key::Enter)]);
    CHECK(frame.keysReleased[toIndex(Key::Enter)]);
}

TEST_CASE("platform/sdl_input_collector: mouse buttons, captured motion and wheel", "[platform][collector]") {
    SdlInputCollector collector;
    // A free cursor never turns the camera.
    collector.handleEvent(rv::test::mouseMotionEvent(40.0f, -10.0f));
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.mouseDelta == glm::vec2(0.0f));

    collector.setMouseCaptured(true);
    CHECK(collector.mouseCaptured());
    collector.handleEvent(rv::test::mouseMotionEvent(3.5f, 2.0f));
    collector.handleEvent(rv::test::mouseMotionEvent(-1.0f, 4.0f));
    collector.handleEvent(rv::test::mouseWheelEvent(1.0f));
    collector.handleEvent(rv::test::mouseWheelEvent(1.0f, SDL_MOUSEWHEEL_FLIPPED)); // natural scrolling undone
    collector.handleEvent(mouseButtonEvent(SDL_BUTTON_RIGHT, true));
    frame = collector.endFrame();
    CHECK(frame.mouseDelta == glm::vec2(2.5f, 6.0f)); // accumulated, +Y down as SDL reports it
    CHECK(frame.mouseWheel == 0.0f);
    CHECK(frame.mouseButtonsPressed[toIndex(MouseButton::Right)]);
    CHECK(frame.mouseButtonsDown[toIndex(MouseButton::Right)]);

    frame = collector.endFrame();
    CHECK(frame.mouseDelta == glm::vec2(0.0f)); // per frame
    CHECK(frame.mouseButtonsDown[toIndex(MouseButton::Right)]);
    collector.handleEvent(mouseButtonEvent(SDL_BUTTON_RIGHT, false));
    collector.handleEvent(mouseButtonEvent(9, true)); // no rv::input code
    frame = collector.endFrame();
    CHECK(frame.mouseButtonsReleased[toIndex(MouseButton::Right)]);
    CHECK(frame.mouseButtonsDown.none());
}

TEST_CASE("platform/sdl_input_collector: focus loss releases everything held", "[platform][collector]") {
    SdlInputCollector collector;
    collector.handleEvent(keyEvent(SDL_SCANCODE_W, true));
    collector.handleEvent(keyEvent(SDL_SCANCODE_LSHIFT, true));
    collector.handleEvent(mouseButtonEvent(SDL_BUTTON_LEFT, true));
    static_cast<void>(collector.endFrame());

    collector.handleEvent(rv::test::windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.keysDown.none());
    CHECK(frame.keysReleased[toIndex(Key::W)]);
    CHECK(frame.keysReleased[toIndex(Key::LeftShift)]);
    CHECK(frame.mouseButtonsReleased[toIndex(MouseButton::Left)]);

    // The late key-up SDL sends for a key released during the focus change is harmless, and keys work again.
    collector.handleEvent(keyEvent(SDL_SCANCODE_W, false));
    collector.handleEvent(keyEvent(SDL_SCANCODE_W, true));
    frame = collector.endFrame();
    CHECK(frame.keysPressed[toIndex(Key::W)]);
    CHECK(frame.keysDown[toIndex(Key::W)]);

    // Through the input system: Sprint was held, then released by the focus loss.
    rv::input::InputSystem system;
    SdlInputCollector other;
    other.handleEvent(keyEvent(SDL_SCANCODE_LSHIFT, true));
    CHECK(system.resolve(other.endFrame()).held(rv::input::Action::Sprint));
    other.releaseAll();
    CHECK(system.resolve(other.endFrame()).released(rv::input::Action::Sprint));
}

TEST_CASE("platform/sdl_input_collector: input of pads that are not open is ignored", "[platform][collector]") {
    SdlInputCollector collector;
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.lastActiveDevice == DeviceKind::KeyboardMouse);
    CHECK_FALSE(frame.gamepadConnected);

    // SDL only reports pads that were opened; anything else (a stale id) changes nothing.
    collector.handleEvent(rv::test::gamepadButtonEvent(42, SDL_GAMEPAD_BUTTON_SOUTH, true));
    frame = collector.endFrame();
    CHECK(frame.lastActiveDevice == DeviceKind::KeyboardMouse);
    CHECK_FALSE(frame.gamepadConnected);
    CHECK(frame.gamepadButtonsDown.none());
    CHECK(collector.gamepads().count() == 0);
}
