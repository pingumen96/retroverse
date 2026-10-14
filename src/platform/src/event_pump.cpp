#include <rv/platform/event_pump.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

#include <SDL3/SDL_events.h>

namespace rv::platform {

void EventPump::poll(const Handler& handler) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (handler) {
            handler(event);
        }
    }
}

void EventPump::waitForEvents(std::chrono::milliseconds timeout) {
    const auto clamped = std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<Sint32>::max());
    SDL_WaitEventTimeout(nullptr, static_cast<Sint32>(clamped));
}

} // namespace rv::platform
