#include <rv/platform/dialogs.hpp>

#include <string>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_messagebox.h>
#include <spdlog/spdlog.h>

namespace rv::platform {

void showErrorDialog(std::string_view title, std::string_view message) {
    const std::string titleText(title);
    const std::string messageText(message);
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, titleText.c_str(), messageText.c_str(), nullptr)) {
        spdlog::warn("Could not show the error dialog: {}", SDL_GetError());
    }
}

} // namespace rv::platform
