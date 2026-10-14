#pragma once

#include <chrono>
#include <functional>

union SDL_Event;

namespace rv::platform {

/// Main-thread access to the SDL event queue.
class EventPump {
public:
    using Handler = std::function<void(const SDL_Event&)>;

    /// Drains every queued event, calling `handler` for each in arrival order (an empty handler discards them).
    /// Exceptions thrown by the handler propagate to the caller; remaining events stay queued.
    void poll(const Handler& handler);

    /// Blocks until an event is queued or `timeout` elapses, without removing the event. Used to idle while the
    /// window is minimized instead of spinning.
    void waitForEvents(std::chrono::milliseconds timeout);
};

} // namespace rv::platform
