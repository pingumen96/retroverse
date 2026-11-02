#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// What the emulators screen (ADR-0018 §5) and the catalog worker exchange: the screen's rows made from the launch table
// (library::SystemLaunchRow) and the launch verifications, a launch test's choice and how the test ended, a change of
// what is stored. A verification counts only for the program it ran.

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/core/expected.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/library/launch_table.hpp>
#include <rv/ui/emulators_model.hpp>

namespace rv::app {

/// The program each verification ran: (system, choice kind, choice value) to the program.
using VerifiedPrograms = std::map<std::tuple<std::string, int, std::string>, std::filesystem::path>;

[[nodiscard]] std::tuple<std::string, int, std::string> verificationKey(const catalog::SystemName& system,
                                                                        const launcher::LaunchChoice& choice);

/// `choice` of `system` was verified, and with the program `translation` finds now: a verification of another program
/// says nothing of this one (ADR-0018 §5).
[[nodiscard]] bool verifiedWith(const VerifiedPrograms& verified, const catalog::SystemName& system,
                                const launcher::LaunchChoice& choice, const catalog::LaunchTranslation& translation);

/// The screen's row of `row`, `games` games, its options the ready ones first; `stored`: the database holds a choice for
/// the system.
[[nodiscard]] ui::EmulatorSystem emulatorSystemOf(const library::SystemLaunchRow& row, std::size_t games,
                                                  const VerifiedPrograms& verified, bool stored);

/// The choice a launch test launches with, and for what it would be stored.
struct LaunchTestChoice {
    launcher::LaunchChoice choice;
    /// Stored for this game alone (a title's choice); nothing: for the game's system.
    std::optional<catalog::GameId> forGame;

    bool operator==(const LaunchTestChoice&) const = default;
};

/// How a launch test ended (CatalogResults::tests).
struct LaunchTestResult {
    std::uint64_t token = 0;
    /// A key of the document: "verified", "crashed", "failed", "quick-exit", "left-running", "blocked", "guard", "error".
    std::string verdict;
    /// The choice was stored: the test returned normally.
    bool stored = false;
    /// The session's id, 0 when none was stored.
    std::int64_t session = 0;

    bool operator==(const LaunchTestResult&) const = default;
};

/// What a finished test's session says, a test that returned normally and could not be stored being "error".
[[nodiscard]] std::string testVerdictOf(const catalog::PlaySession& session, bool stored);

/// A change of what is stored, on the worker.
struct ChoiceChange {
    enum class Kind : std::uint8_t {
        Save,         ///< `choice` for `system`, without a test ("Salva senza provare")
        Clear,        ///< the choice stored for `system` removed
        SetProgram,   ///< `program` for `emulator`, checked first (esde::checkProgramChoice())
        ClearProgram, ///< the program of `emulator` removed
    };
    Kind kind = Kind::Save;
    catalog::SystemName system;
    launcher::LaunchChoice choice;
    std::string emulator;
    std::filesystem::path program;
};

/// A change ended: `problem` "" when it went well, else a key of the document ("refused", "missing", "write").
struct ChoiceChanged {
    std::uint64_t serial = 0;
    ChoiceChange::Kind kind = ChoiceChange::Kind::Save;
    std::string problem;
};

/// The table the worker answered (nothing asked of it can fail but this): the systems, or why not.
struct EmulatorTable {
    std::uint64_t serial = 0;
    std::optional<Error> error;
    std::vector<ui::EmulatorSystem> systems;
    /// How long the worker took (ADR-0018 §16: shown within 1 s of opening).
    double milliseconds = 0.0;
};

/// A system opened: its options and the games a launch test of it offers.
struct EmulatorSamples {
    std::uint64_t serial = 0;
    std::string system;
    std::vector<ui::EmulatorOption> options;
    std::vector<ui::EmulatorSample> games;
};

} // namespace rv::app
