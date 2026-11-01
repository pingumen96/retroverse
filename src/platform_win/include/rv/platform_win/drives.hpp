#pragma once

// The machine's drive letters (I7 step 2: the wizard's folder browser). Asks Windows for the letters only: no volume is
// touched, so a slow or missing one costs nothing here.

#include <filesystem>
#include <vector>

namespace rv::platform_win {

/// The root of every drive letter in use ("C:\", ...), by letter.
[[nodiscard]] std::vector<std::filesystem::path> driveRoots();

} // namespace rv::platform_win
