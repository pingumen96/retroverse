#include <rv/app/back_action.hpp>

namespace rv::app {

BackAction backAction(const BackState& state) noexcept {
    if (state.overlayVisible && state.overlayHasKeyboard) {
        return BackAction::Ignore;
    }
    if (state.overlayVisible && state.overlayPopupOpen) {
        return BackAction::ClosePopup;
    }
    if (state.mouseCaptured) {
        return BackAction::ReleaseMouse;
    }
    if (state.overlayVisible) {
        return BackAction::CloseOverlay;
    }
    return BackAction::Quit;
}

} // namespace rv::app
