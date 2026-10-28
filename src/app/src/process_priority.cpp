#include <rv/app/process_priority.hpp>

namespace rv::app {

PriorityChoice processPriorityFor(bool minimized, bool gameRunning) noexcept {
    if (gameRunning) {
        return PriorityChoice{false, "launch"};
    }
    if (minimized) {
        return PriorityChoice{false, "minimized"};
    }
    return PriorityChoice{true, "rendering"};
}

} // namespace rv::app
