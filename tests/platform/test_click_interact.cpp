// ADR-0012 §2's click rule through the input pipeline as rv::app::AppInput routes it (I3 step 13a): the mouse capture
// first (the click that captures goes no further), then the collector, the input system and the default bindings, whose
// left mouse button is `interact`. A left click while the mouse is free only captures it; once captured, a left click
// is interact.

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/input_system.hpp>
#include <rv/platform/mouse_capture.hpp>
#include <rv/platform/sdl_input_collector.hpp>

#include "sdl_test_events.hpp"

namespace {

/// AppInput's routing, without the window's gamepads and the input script.
class Pipeline {
public:
    explicit Pipeline(SDL_Window* window) : m_capture(window) {}

    void handleEvent(const SDL_Event& event) {
        const bool consumed = m_capture.handleEvent(event);
        m_collector.setMouseCaptured(m_capture.captured());
        if (!consumed) {
            m_collector.handleEvent(event);
        }
    }

    [[nodiscard]] rv::input::ActionState frame() { return m_system.resolve(m_collector.endFrame()); }
    [[nodiscard]] bool captured() const noexcept { return m_capture.captured(); }
    void release() {
        static_cast<void>(m_capture.setCaptured(false, "Back pressed"));
        m_collector.setMouseCaptured(m_capture.captured());
    }

private:
    rv::platform::MouseCapture m_capture;
    rv::platform::SdlInputCollector m_collector;
    rv::input::InputSystem m_system{rv::input::ActionMap::defaults()};
};

} // namespace

TEST_CASE("platform/click_interact: a click while the mouse is free only captures it, once captured it is interact",
          "[platform][mouse_capture]") {
    rv::test::HiddenWindowScope sdl;
    if (sdl.window() == nullptr) {
        SKIP("SDL video unavailable: " << SDL_GetError());
    }
    const SDL_WindowID id = sdl.id();
    Pipeline input(sdl.window());
    input.handleEvent(rv::test::windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, id));
    CHECK_FALSE(input.frame().pressed(rv::input::Action::Interact));

    // The mouse is free: the click captures it and never reaches the action map.
    input.handleEvent(rv::test::mouseButtonEvent(SDL_BUTTON_LEFT, true, id));
    input.handleEvent(rv::test::mouseButtonEvent(SDL_BUTTON_LEFT, false, id));
    CHECK(input.captured());
    rv::input::ActionState actions = input.frame();
    CHECK_FALSE(actions.pressed(rv::input::Action::Interact));
    CHECK_FALSE(actions.held(rv::input::Action::Interact));

    // Captured: the next left click is interact.
    input.handleEvent(rv::test::mouseButtonEvent(SDL_BUTTON_LEFT, true, id));
    actions = input.frame();
    CHECK(actions.pressed(rv::input::Action::Interact));
    input.handleEvent(rv::test::mouseButtonEvent(SDL_BUTTON_LEFT, false, id));
    CHECK(input.frame().released(rv::input::Action::Interact));

    // Freed again (Back): the next click only captures it once more.
    input.release();
    CHECK_FALSE(input.captured());
    input.handleEvent(rv::test::mouseButtonEvent(SDL_BUTTON_LEFT, true, id));
    CHECK(input.captured());
    CHECK_FALSE(input.frame().pressed(rv::input::Action::Interact));
}
