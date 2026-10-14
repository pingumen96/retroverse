#pragma once

#include <string_view>

namespace rv::platform {

/// Shows a modal error message box (SDL_ShowSimpleMessageBox) and blocks until it is dismissed. Works before SDL is
/// initialized and without a window. Text is UTF-8. A failure to show the box is logged, never fatal.
void showErrorDialog(std::string_view title, std::string_view message);

} // namespace rv::platform
