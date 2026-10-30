#pragma once

// A game's manual opened in Windows' default PDF program (I5 step 8, ADR-0016 §10): the media viewer's "Apri il
// manuale" starts explorer.exe with the manual's file as its one argument, through an IProcessRunner (Win32ProcessRunner
// in the product: no shell, argv, the automation guard applied before anything starts). It is no game session: nothing
// waits for it and Retroverse keeps running.
//
// explorer.exe is one of the programs no emulator profile may start (isRefusedProgram: it reads its command line its
// own way). This plan is the one exception, and it keeps to what explorer.exe reads as one path: the manual's plain
// absolute path (a \\?\ form given back as the drive or UNC path it stands for), a .pdf file, within MAX_PATH, no element
// that Windows' path normalization would change (a trailing dot or space) or that names a device (a reserved DOS name,
// whatever its extension), no colon past the drive's (an alternate data stream), and no comma (explorer.exe splits its
// argument at its commas; the argument is always quoted, but whether explorer.exe keeps a quoted comma cannot be proved
// without opening a window). Every other character, '&', '%', '^', '!', quotes aside (no Windows name holds one), stays
// in the one argument: CreateProcessW runs no interpreter.

#include <filesystem>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/launcher/launch_request.hpp>

namespace rv::launcher {

/// The program that opens a manual, in the Windows folder (ISystemProbe::windowsFolder()).
inline constexpr std::string_view kManualOpenerProgram = "explorer.exe";
/// The extension a manual has (esde::kManualExtensions), compared ignoring case.
inline constexpr std::string_view kManualExtension = ".pdf";
/// The longest plain path explorer.exe is given, in UTF-16 code units (MAX_PATH less its terminating null).
inline constexpr std::size_t kMaxManualPathUnits = 259;

/// The argument explorer.exe is given for `manual`: its plain absolute path in UTF-8 ("C:\..." or "\\server\share\...";
/// a "\\?\" or "\\?\UNC\" form given back as that). Errors (Unsupported, naming the rule): not absolute, a device path
/// (\\.\), not a .pdf, longer than kMaxManualPathUnits, an element ending with a dot or a space, an element whose stem
/// is a reserved device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, COM and LPT with ¹, ² or ³, any case and extension), a
/// colon past the drive's, a comma; InvalidArgument for a path that is not UTF-8.
[[nodiscard]] Expected<std::string> manualArgument(const std::filesystem::path& manual);

/// The request that opens `manual`: `probe`'s Windows folder's explorer.exe, manualArgument() as its one argument (always
/// in double quotes: quoteEveryArgument), run in the Windows folder, waited for as a direct process with no startup
/// grace and no window timeout (it hands the file to the shell and exits), labelled `label`. Errors: manualArgument()'s,
/// and validateLaunchRequest()'s (explorer.exe missing or no program). Reads the file system (explorer.exe's header), never the manual; starts nothing: the
/// automation guard applies when the request starts (launch_guard.hpp), and refuses it under RV_LAUNCH_ALLOWLIST unless
/// explorer.exe is listed.
[[nodiscard]] Expected<LaunchRequest> planManualOpen(const std::filesystem::path& manual,
                                                     const core::ISystemProbe& probe, std::string label = {});

} // namespace rv::launcher
