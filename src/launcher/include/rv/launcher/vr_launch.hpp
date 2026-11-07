#pragma once

// A launch adapted to its program (I11 step 12, ADR-0020 §1). Whichever ES-DE command or Retroverse profile made a
// LaunchRequest, its executable's file name names a program of emulators.json (EmulatorProfiles::programFor()):
// - the program's window rules apply to every launch (the window followed, window_choice.hpp);
// - in VR, its arguments are dropped and added and its append-file is passed with the program's option, so that the
//   game shows in a window VR can capture (windowed or borderless, never exclusive fullscreen) and goes on when it
//   loses the focus.
// Retroverse never writes a program's own configuration: the append-file is Retroverse's, under
// <user data>/launcher/vr (core::Paths::vrLaunchDir()). GPU-free; only writeAppendFile() writes.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_request.hpp>

namespace rv::launcher {

/// The append-file of a launch in VR: where it goes and what it holds.
struct AppendFile {
    std::filesystem::path path;
    std::string bytes;

    friend bool operator==(const AppendFile&, const AppendFile&) = default;
};

/// What adaptLaunch() made of a request.
struct AdaptedLaunch {
    /// The request to start.
    LaunchRequest request;
    /// The program matched; empty when none did (the request is unchanged).
    std::string programId;
    std::string programName;
    /// A launch in VR.
    bool vr = false;
    /// In VR: the arguments added (in their order, the append-file's option and path among them), and those dropped.
    std::vector<std::string> added;
    std::vector<std::string> dropped;
    /// In VR: the file to write before the program starts (writeAppendFile()).
    std::optional<AppendFile> appendFile;
    /// In VR: how much of the GPU the game takes.
    InGameLoad inGame = InGameLoad::Light;
    /// In VR: why the program's VR settings were not applied (the request they made was refused, or the append-file's
    /// path is not UTF-8 text); the request is then the one asked for, its window rules aside.
    std::optional<std::string> refused;
};

/// The append-file's bytes: each line followed by a line feed.
[[nodiscard]] std::string appendFileBytes(const AppendConfig& config);

/// `request` adapted to `program` (null: unchanged). The program's window rules replace the request's; when `vr`:
///  1. the arguments equal to one of VrSettings::dropArguments are removed;
///  2. the append-file is <vrFolder>/<file>: when the launch already passes its option followed by a value and the
///     program joins values, that value gets AppendConfig::joinWith and the file's path appended; otherwise the option
///     and the path go in front;
///  3. VrSettings::addArguments go first, before the option and the path;
///  4. the load is heavy when the program's is, or when an argument's file name (after its last '\' or '/') matches
///     one of VrSettings::heavyWith;
///  5. the request must still pass validateLaunchRequest(); when it does not, the one asked for is kept (refused says
///     why).
[[nodiscard]] AdaptedLaunch adaptLaunch(LaunchRequest request, const ProgramProfile* program, bool vr,
                                        const std::filesystem::path& vrFolder);

/// Writes `file`, creating its folder, unless it already holds exactly those bytes (an atomic replacement otherwise).
/// Errors: IoError, naming the file.
[[nodiscard]] Status writeAppendFile(const AppendFile& file);

} // namespace rv::launcher
