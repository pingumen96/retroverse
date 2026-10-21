#include <rv/app/back_action.hpp>

namespace rv::app {

std::string_view toString(BackAction action) noexcept {
    switch (action) {
    case BackAction::Ignore:
        return "ignore";
    case BackAction::ClosePopup:
        return "close popup";
    case BackAction::PanelBack:
        return "panel back";
    case BackAction::CatalogBack:
        return "catalog back";
    case BackAction::DirectoryBack:
        return "directory back";
    case BackAction::ReleaseMouse:
        return "release mouse";
    case BackAction::CloseOverlay:
        return "close overlay";
    case BackAction::Quit:
        return "quit";
    }
    return "unknown";
}

BackAction backAction(const BackState& state) noexcept {
    if (state.overlayVisible && state.overlayHasKeyboard) {
        return BackAction::Ignore;
    }
    if (state.overlayVisible && state.overlayPopupOpen) {
        return BackAction::ClosePopup;
    }
    if (state.panelOpen) {
        return BackAction::PanelBack;
    }
    if (state.catalogOpen) {
        return BackAction::CatalogBack;
    }
    if (state.directoryOpen) {
        return BackAction::DirectoryBack;
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
