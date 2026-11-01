#include <catch2/catch_test_macros.hpp>

#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keycode.h>

#include <rv/input/action.hpp>
#include <rv/input/codes.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/platform/gamepad_manager.hpp>
#include <rv/platform/sdl_input_collector.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/settings_model.hpp>

#include "sdl_test_events.hpp"

using rv::input::DeviceKind;
using rv::input::GamepadAxis;
using rv::input::GamepadButton;
using rv::input::toIndex;
using rv::platform::SdlInputCollector;

// Hot-plug through SDL's virtual joystick driver: real SDL events, no hardware. VirtualGamepadScope hides the
// controllers plugged into the machine, so the only pads are the ones attached here.

namespace {

/// Delivers every queued SDL event (joysticks are updated by the pump) to the collector.
void pump(SdlInputCollector& collector) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        collector.handleEvent(event);
    }
}

/// Test-side handle on a virtual pad, to press its buttons and move its axes.
class VirtualPad {
public:
    explicit VirtualPad(SDL_JoystickID id) : m_joystick(SDL_OpenJoystick(id)) {}
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
    ~VirtualPad() {
        if (m_joystick != nullptr) {
            SDL_CloseJoystick(m_joystick);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return m_joystick != nullptr; }
    bool button(SDL_GamepadButton button, bool down) { return SDL_SetJoystickVirtualButton(m_joystick, button, down); }
    bool axis(SDL_GamepadAxis axis, Sint16 value) { return SDL_SetJoystickVirtualAxis(m_joystick, axis, value); }

private:
    SDL_Joystick* m_joystick = nullptr;
};

} // namespace

TEST_CASE("platform/gamepad_manager: hot-plugged pads report buttons and axes in rv::input conventions",
          "[platform][gamepad]") {
    rv::test::VirtualGamepadScope sdl;
    if (!sdl.ready()) {
        SKIP("SDL gamepad subsystem unavailable: " << SDL_GetError());
    }
    SdlInputCollector collector;
    pump(collector);
    REQUIRE(collector.gamepads().count() == 0);

    const SDL_JoystickID id = sdl.attach("Retroverse Test Pad");
    REQUIRE(id != 0);
    pump(collector); // SDL_EVENT_GAMEPAD_ADDED opens it
    REQUIRE(collector.gamepads().isOpen(id));
    CHECK(collector.gamepads().count() == 1);
    CHECK(collector.gamepads().activeId() == id); // the only pad is active at once
    CHECK(collector.gamepads().name(id) == "Retroverse Test Pad");

    VirtualPad pad(id);
    REQUIRE(pad.valid());
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_EAST, true));
    REQUIRE(pad.axis(SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN)); // stick pushed away from the player
    REQUIRE(pad.axis(SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MAX));
    REQUIRE(pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX));
    pump(collector);
    rv::input::RawInputFrame frame = collector.endFrame();
    REQUIRE(frame.gamepadConnected);
    CHECK(frame.lastActiveDevice == DeviceKind::Gamepad);
    CHECK(frame.gamepadButtonsDown[toIndex(GamepadButton::East)]);
    CHECK(frame.gamepadButtonsPressed[toIndex(GamepadButton::East)]);
    CHECK(frame.gamepadAxes[toIndex(GamepadAxis::LeftY)] == 1.0f); // +Y up
    CHECK(frame.gamepadAxes[toIndex(GamepadAxis::RightX)] == 1.0f);
    CHECK(frame.gamepadAxes[toIndex(GamepadAxis::RightTrigger)] == 1.0f);
    CHECK(frame.gamepadAxes[toIndex(GamepadAxis::LeftTrigger)] == 0.0f);

    // Through the default bindings: B is Back, the left stick walks forward.
    rv::input::InputSystem system;
    const rv::input::ActionState actions = system.resolve(frame);
    CHECK(actions.pressed(rv::input::Action::Back));
    CHECK(actions.axis(rv::input::Action::MoveForward) == 1.0f);
    CHECK(actions.activeDevice() == DeviceKind::Gamepad);

    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_EAST, false));
    REQUIRE(pad.axis(SDL_GAMEPAD_AXIS_LEFTY, 0));
    pump(collector);
    frame = collector.endFrame();
    CHECK(frame.gamepadButtonsReleased[toIndex(GamepadButton::East)]);
    CHECK_FALSE(frame.gamepadButtonsDown[toIndex(GamepadButton::East)]);
    CHECK(frame.gamepadAxes[toIndex(GamepadAxis::LeftY)] == 0.0f);

    // A key press makes the keyboard the last active device again.
    collector.handleEvent(rv::test::keyEvent(SDL_SCANCODE_E, true));
    CHECK(collector.endFrame().lastActiveDevice == DeviceKind::KeyboardMouse);

    // So do a wheel notch and captured mouse motion after the pad was used; the motion of a free cursor does not.
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true));
    pump(collector);
    REQUIRE(collector.endFrame().lastActiveDevice == DeviceKind::Gamepad);
    collector.handleEvent(rv::test::mouseWheelEvent(1.0f));
    CHECK(collector.endFrame().lastActiveDevice == DeviceKind::KeyboardMouse);

    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false));
    pump(collector);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true));
    pump(collector);
    REQUIRE(collector.endFrame().lastActiveDevice == DeviceKind::Gamepad);
    collector.handleEvent(rv::test::mouseMotionEvent(5.0f, 0.0f));
    CHECK(collector.endFrame().lastActiveDevice == DeviceKind::Gamepad);
    collector.setMouseCaptured(true);
    collector.handleEvent(rv::test::mouseMotionEvent(5.0f, 0.0f));
    CHECK(collector.endFrame().lastActiveDevice == DeviceKind::KeyboardMouse);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false));
    pump(collector);

    REQUIRE(SDL_DetachVirtualJoystick(id));
    pump(collector); // SDL_EVENT_GAMEPAD_REMOVED closes it
    CHECK_FALSE(collector.gamepads().isOpen(id));
    CHECK(collector.gamepads().count() == 0);
    CHECK_FALSE(collector.gamepads().activeId().has_value());
    CHECK_FALSE(collector.endFrame().gamepadConnected);
}

TEST_CASE("platform/gamepad_manager: ten unplug and replug cycles", "[platform][gamepad]") {
    rv::test::VirtualGamepadScope sdl;
    if (!sdl.ready()) {
        SKIP("SDL gamepad subsystem unavailable: " << SDL_GetError());
    }
    SdlInputCollector collector;
    for (int cycle = 0; cycle < 10; ++cycle) {
        INFO("cycle " << cycle);
        const SDL_JoystickID id = sdl.attach("Replugged Pad");
        REQUIRE(id != 0);
        pump(collector);
        REQUIRE(collector.gamepads().activeId() == id);
        {
            VirtualPad pad(id);
            REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true));
            pump(collector);
            CHECK(collector.endFrame().gamepadButtonsDown[toIndex(GamepadButton::South)]);
        }
        // Unplugged while the button is held: the pad disappears from the frame, nothing stays stuck.
        REQUIRE(SDL_DetachVirtualJoystick(id));
        pump(collector);
        CHECK(collector.gamepads().count() == 0);
        const rv::input::RawInputFrame frame = collector.endFrame();
        CHECK_FALSE(frame.gamepadConnected);
        CHECK(frame.gamepadButtonsDown.none());
    }
}

TEST_CASE("platform/gamepad_manager: the most recently used pad is active", "[platform][gamepad]") {
    rv::test::VirtualGamepadScope sdl;
    if (!sdl.ready()) {
        SKIP("SDL gamepad subsystem unavailable: " << SDL_GetError());
    }
    SdlInputCollector collector;
    const SDL_JoystickID first = sdl.attach("First Pad");
    pump(collector);
    const SDL_JoystickID second = sdl.attach("Second Pad");
    pump(collector);
    REQUIRE(collector.gamepads().count() == 2);
    CHECK(collector.gamepads().activeId() == first); // connecting does not steal the active role

    VirtualPad firstPad(first);
    VirtualPad secondPad(second);
    // A resting trigger and a small stick drift are not "use".
    REQUIRE(secondPad.axis(SDL_GAMEPAD_AXIS_LEFTX, 6000));
    pump(collector);
    CHECK(collector.gamepads().activeId() == first);

    REQUIRE(secondPad.button(SDL_GAMEPAD_BUTTON_NORTH, true));
    pump(collector);
    CHECK(collector.gamepads().activeId() == second);
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.gamepadButtonsPressed[toIndex(GamepadButton::North)]);

    // A stick pushed past the activity threshold switches back; the frame shows that pad's state only.
    REQUIRE(firstPad.axis(SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MAX));
    pump(collector);
    CHECK(collector.gamepads().activeId() == first);
    frame = collector.endFrame();
    CHECK(frame.gamepadAxes[toIndex(GamepadAxis::RightY)] == -1.0f);
    CHECK_FALSE(frame.gamepadButtonsDown[toIndex(GamepadButton::North)]); // held on the other pad

    // Removing the active pad hands over to the remaining one.
    REQUIRE(SDL_DetachVirtualJoystick(first));
    pump(collector);
    CHECK(collector.gamepads().activeId() == second);
    CHECK(collector.endFrame().gamepadButtonsDown[toIndex(GamepadButton::North)]);

    // Pads already connected are opened on request, once.
    rv::platform::GamepadManager late;
    late.openConnected();
    CHECK(late.count() == 1);
    CHECK(late.isOpen(second));
    late.openConnected();
    CHECK(late.count() == 1);
    late.closeAll();
    CHECK(late.count() == 0);
    CHECK_FALSE(late.activeId().has_value());
}

namespace {

constexpr double kFrameSeconds = 1.0 / 60.0;

/// The application's path from the pad to a UI screen: the collector's frame through the bindings and the UI's navigation
/// into the settings screen's model.
class UiScreen {
public:
    UiScreen() {
        rv::ui::SettingsRow first;
        first.key = "first";
        first.group = "controls";
        first.kind = rv::ui::SettingsRowKind::Toggle;
        first.value = false;
        rv::ui::SettingsRow second = first;
        second.key = "second";
        rv::ui::SettingsRow third = first;
        third.key = "third";
        m_model.setRows({first, second, third});
        m_model.open();
    }

    /// One frame: what the collector says now goes through to the model. The frame's commands.
    std::size_t frame(SdlInputCollector& collector) {
        pump(collector);
        const rv::input::RawInputFrame raw = collector.endFrame();
        m_connected = raw.gamepadConnected;
        const rv::input::ActionState actions = m_system.resolve(raw);
        m_commands.clear();
        m_nav.update(actions, kFrameSeconds, m_commands);
        for (const rv::ui::UiCommand command : m_commands.commands()) {
            static_cast<void>(m_model.apply(command));
        }
        return m_commands.commands().size();
    }

    [[nodiscard]] std::size_t focus() const noexcept { return m_model.focus(); }
    [[nodiscard]] bool connected() const noexcept { return m_connected; }

private:
    rv::input::InputSystem m_system;
    rv::ui::NavInput m_nav;
    rv::ui::UiCommandList m_commands;
    rv::ui::SettingsModel m_model;
    bool m_connected = false;
};

} // namespace

TEST_CASE("platform/gamepad_manager: a pad unplugged and back during a UI screen (FR-029)", "[platform][gamepad]") {
    rv::test::VirtualGamepadScope sdl;
    if (!sdl.ready()) {
        SKIP("SDL gamepad subsystem unavailable: " << SDL_GetError());
    }
    SdlInputCollector collector;
    UiScreen screen;
    SDL_JoystickID id = sdl.attach("Sofa Pad");
    REQUIRE(id != 0);
    screen.frame(collector);
    REQUIRE(screen.connected());
    {
        VirtualPad pad(id);
        REQUIRE(pad.valid());
        REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true));
        screen.frame(collector);
        CHECK(screen.focus() == 1);
    }
    // Unplugged with the D-pad held: the focus does not run on by itself, for all the repeat's delay and more.
    REQUIRE(SDL_DetachVirtualJoystick(id));
    screen.frame(collector);
    CHECK_FALSE(screen.connected());
    for (int frame = 0; frame < 120; ++frame) {
        CHECK(screen.frame(collector) == 0);
    }
    CHECK(screen.focus() == 1);

    // Back, a new pad id: the screen follows it at once, nothing left held.
    id = sdl.attach("Sofa Pad");
    REQUIRE(id != 0);
    screen.frame(collector);
    CHECK(screen.connected());
    VirtualPad pad(id);
    REQUIRE(pad.valid());
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true));
    screen.frame(collector);
    CHECK(screen.focus() == 2);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false));
    screen.frame(collector);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, true));
    screen.frame(collector);
    CHECK(screen.focus() == 1);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, false));
    REQUIRE(SDL_DetachVirtualJoystick(id));
    pump(collector);
}

TEST_CASE("platform/gamepad_manager: a pad unplugged and back across a game (FR-029)", "[platform][gamepad]") {
    rv::test::VirtualGamepadScope sdl;
    if (!sdl.ready()) {
        SKIP("SDL gamepad subsystem unavailable: " << SDL_GetError());
    }
    SdlInputCollector collector;
    SDL_JoystickID id = sdl.attach("Game Pad");
    REQUIRE(id != 0);
    pump(collector);
    {
        VirtualPad pad(id);
        REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true));
        pump(collector);
        REQUIRE(collector.endFrame().gamepadButtonsDown[toIndex(GamepadButton::South)]);
    }
    // A game has the screen: the application drops what droppedWhileIdle() names and takes the rest, the pad's coming and
    // going among them. It went and came back meanwhile, the button that launched still down when it left.
    REQUIRE(SDL_DetachVirtualJoystick(id));
    id = sdl.attach("Game Pad");
    REQUIRE(id != 0);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.type != SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            collector.handleEvent(event);
        }
    }
    collector.releaseAll(); // AppInput::releaseAll() after the wait
    rv::input::RawInputFrame frame = collector.endFrame();
    CHECK(frame.gamepadConnected);
    CHECK(frame.gamepadButtonsDown.none());
    CHECK(collector.gamepads().count() == 1);
    CHECK(collector.gamepads().activeId() == id);

    // The return: the same pad answers.
    VirtualPad pad(id);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_EAST, true));
    pump(collector);
    frame = collector.endFrame();
    CHECK(frame.gamepadButtonsPressed[toIndex(GamepadButton::East)]);
    CHECK(frame.lastActiveDevice == DeviceKind::Gamepad);
    REQUIRE(pad.button(SDL_GAMEPAD_BUTTON_EAST, false));
    pump(collector);
    REQUIRE(SDL_DetachVirtualJoystick(id));
    pump(collector);
}
