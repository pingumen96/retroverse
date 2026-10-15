#include <rv/app/back_action.hpp>

namespace rv::app {

BackAction backAction(bool mouseCaptured, bool overlayVisible, bool overlayHasKeyboard) noexcept {
    if (overlayVisible && overlayHasKeyboard) {
        return BackAction::Ignore;
    }
    if (mouseCaptured) {
        return BackAction::ReleaseMouse;
    }
    if (overlayVisible) {
        return BackAction::CloseOverlay;
    }
    return BackAction::Quit;
}

} // namespace rv::app
