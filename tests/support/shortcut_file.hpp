#pragma once

// Windows shortcuts (.lnk files) made at test time through IShellLinkW (I11 step 13): what ES-DE's "open the game's
// file" games are, for the translation's and the app's tests. Never a real program's shortcut: the tests point them at
// program stubs or at the fake emulator.

#include <filesystem>
#include <string>

#include <rv/core/expected.hpp>

namespace rv::test {

/// Writes the shortcut `file` opening `target` with `arguments` (one string, as the shell passes it on) in
/// `workingDirectory` (empty: none), its "Run as administrator" flag `administrator`. Errors: PlatformError.
Status writeShortcut(const std::filesystem::path& file, const std::filesystem::path& target,
                     const std::string& arguments = {}, const std::filesystem::path& workingDirectory = {},
                     bool administrator = false);

} // namespace rv::test
