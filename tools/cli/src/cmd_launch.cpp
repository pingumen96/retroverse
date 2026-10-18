// retroverse-cli launch (I2 step 10: --dry-run; I2 step 12: --game with --wait and --test).
//
// `launch --game <id>` launches a game of the catalog (the GameId `show` and `games` print; an alias stands for the
// game it names) of the library library.json records. Right before the launch it resolves the game as ADR-0008 §8 says
// (catalog::resolveLaunch): the game's file looked for on disk now, its <altemulator> and its system's
// <alternativeEmulator> as the last import read them, the commands translated from the current es_systems.xml and find
// rules, and Retroverse's choices from the catalog database: the game's own (`assign --game`) first, then the system's
// (`assign <system>`, which ranks before emulators.json's "systemChoices" for the same system: the user's latest
// explicit choice). A game that is not ready is not started: its readiness and blockers are printed and the attempt is
// recorded as a blocked session. A ready game starts through launcher::LaunchSession on platform_win::
// Win32ProcessRunner, which applies the automation guard (RV_LAUNCH_ALLOWLIST); the attempt is recorded as a play session
// (catalog_db::SessionStore: UTC times, the choice, the outcome, this process as its owner) when it begins and again when
// it ends. Nothing is ever terminated:
//  - without --wait, the launch is followed through its startup grace (2 s and a margin), so that a failure to start, a
//    crash or a quick exit is recorded as such; a game still running then is recorded as left running (not waited for)
//    and the command returns, the game running on;
//  - --wait follows the session to its end as its wait strategy says (ADR-0006: the started process, the process tree,
//    or the user's confirmation, which Ctrl+C gives), then records the outcome; Ctrl+C while the game runs stops waiting
//    (left running, quit); --wait-limit <seconds> stops waiting after that long (left running, wait limit);
//  - --test waits (--wait) and, when the game returned normally (a clean exit that was no quick exit, nothing left
//    running), records a launch verification for the choice and the system; it exits 1 otherwise. Every launch that
//    returned normally records one too (ADR-0008 §8: "verified" is shown).
// At its start the command closes as interrupted the sessions left open by owners that are gone (ADR-0008 §9). A
// session write that meets a busy database (another process's import) is tried again and kept rather than lost: the
// first write waits at most a second before the game starts anyway, the end is written with two minutes of patience.
//
// `launch --dry-run` resolves the launch of --game, or of the game at --path in the ROM folder of --system (named by its
// path relative to the system's ROM folder, its <altemulator> and title read from its gamelist entry), and prints the
// command or profile chosen and why, its readiness, the program and core with the find rule and entry that found them,
// the blockers, and the request it would start: argv (the program, then each argument as the emulator receives it), the
// working folder, the environment overrides, the console window, the command line, and what the automation guard
// would say of it. The game's path is spelled as the folder lists it, as ES-DE's %ROM% spells it. Nothing is started,
// nothing is written (Retroverse's choices are read from the catalog database when there is one: the system's for
// --path, the game's and the system's for --game). Command lines are logged at debug level only (ADR-0008 §9).
//
// Exit codes: 0 a dry run that is ready; a launch that started and returned (or runs on: not waited for, the wait limit,
// Ctrl+C), for --test only one that returned normally; 1 not ready (blocked, recorded), failed to start, crashed, a quick
// exit, a --test that did not return normally, a --game the catalog does not have; 2 invalid options, an unknown system,
// a path that is not below the system's ROM folder, no library.json, a --game of another library or that is not a game
// (a disc member, a folder, not a game for ES-DE); 3 when ES-DE's configuration, the catalog or the results cannot be
// read or written, or a session could not be recorded.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/platform_win/win32_util.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "launch_text.hpp"
#include "session_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli launch";

/// How long past its startup grace a launch without --wait is followed, so that an exit within the grace is observed.
constexpr std::chrono::milliseconds kGraceMargin{500};
/// The longest --wait-limit, in seconds (a day).
constexpr int kMaxWaitLimitSeconds = 86400;
/// How long the wait sleeps when nothing happens (the runner's events wake it at once).
constexpr std::chrono::milliseconds kWaitSlice{100};
/// How often a session write the database kept busy is tried again while the game runs.
constexpr std::chrono::seconds kPendingRetryInterval{2};

/// What a resolution found (a dry run, or the launch right before it starts).
struct DryRun {
    const catalog::SystemInfo* system = nullptr;
    /// The game of the catalog (--game), and the GameId the command was given (an alias, when it differs).
    std::optional<catalog::GameId> id;
    std::optional<catalog::GameId> requestedId;
    /// The path as given, relative to the ROM folder ('/' between the elements).
    std::string requestedPath;
    catalog::LaunchGame game;
    std::filesystem::path romPath;
    /// The command as es_systems.xml writes it, when an ES-DE command was chosen.
    std::string commandText;
    std::string systemAlternativeEmulator;
    catalog::LaunchResolution resolution;
    /// Where Retroverse's choice came from: kAssignedOrigin (the game's or the system's, from the catalog database),
    /// kEmulatorsJsonOrigin; empty for ES-DE's own choices.
    std::string origin;
    std::optional<launcher::GuardVerdict> guard;
    std::optional<std::string> commandLine;
    /// The system as the catalog holds it, when the library no longer configures it (`system` points here then).
    std::optional<catalog::SystemInfo> catalogSystem;
};

/// The game's availability on disk: its system's folder, then each element of its path as the folder spells it.
/// Error (InvalidArgument): a path no file can have (a wildcard, a character Windows names cannot hold).
Status findGame(const catalog::SystemInfo& system, DryRun& run) {
    run.game.relativePath = run.requestedPath;
    if (system.availability != catalog::Availability::Available) {
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = system.unavailableReason;
        return ok();
    }
    const auto listed = esde::listedPath(system.romFolder, run.requestedPath);
    if (listed) {
        run.game.relativePath = listed->relativePath;
        run.game.directory = listed->directory;
        return ok();
    }
    switch (listed.error().code) {
    case ErrorCode::InvalidArgument:
        return propagate(listed.error());
    case ErrorCode::NotFound:
        run.game.availability = catalog::Availability::Missing;
        break;
    case ErrorCode::Unavailable:
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = catalog::SourceUnavailableReason::Unreachable;
        break;
    default:
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = catalog::SourceUnavailableReason::NotListable;
        break;
    }
    return ok();
}

/// The game's own choice and title from the system's gamelist (the entry ES-DE applies to the file as listed).
void readGameEntry(LaunchLibrary& library, const catalog::SystemInfo& system, DryRun& run,
                   catalog::LaunchChoices& choices) {
    const auto gamelist = readSystemGamelist(library, system);
    run.game.title = catalog::defaultTitle({}, run.game.relativePath, run.game.directory);
    if (!gamelist) {
        return;
    }
    run.systemAlternativeEmulator = gamelist->alternativeEmulator;
    choices.systemAlternativeEmulator = gamelist->alternativeEmulator;
    const auto key = catalog::makeLookupKey(run.game.relativePath);
    if (!key) {
        return;
    }
    std::vector<const esde::GamelistEntry*> sameKey;
    for (const esde::GamelistEntry& entry : gamelist->entries) {
        if (!entry.outsideSystemFolder() && entry.key.key() == key->key()) {
            sameKey.push_back(&entry);
        }
    }
    const esde::GamelistEntry* entry = esde::entryForListed(sameKey, *key, esde::GamelistEntryKind::Game);
    if (entry != nullptr && entry->kind == esde::GamelistEntryKind::Game) {
        run.game.title = catalog::defaultTitle(entry->metadata, run.game.relativePath, run.game.directory);
        choices.gameAltEmulator = entry->altEmulator;
    }
}

/// The command text of the chosen ES-DE command, the guard's verdict and the command line of the request.
void finishResolution(LaunchLibrary& library, DryRun& run) {
    const catalog::ChosenLaunch& chosen = run.resolution.chosen;
    if (chosen.choice.kind == launcher::LaunchChoiceKind::EsdeCommand && !chosen.blocker) {
        if (const esde::SystemDefinition* definition = library.systems.find(run.system->name.str())) {
            if (const esde::SystemCommand* command = definition->findCommand(chosen.choice.value)) {
                run.commandText = command->text;
            }
        }
    }
    if (const auto& request = run.resolution.translation.request) {
        // What an automated run (RV_LAUNCH_ALLOWLIST) says of it; the runner applies it again when it starts.
        run.guard = launcher::LaunchGuard::fromEnvironment().check(*request);
        if (auto commandLine = launcher::commandLineOf(*request)) {
            run.commandLine = std::move(*commandLine);
        }
    }
}

Json requestJson(const launcher::LaunchRequest& request) {
    Json argv = Json::array();
    argv.push_back(pathText(request.executable));
    for (const std::string& argument : request.arguments) {
        argv.push_back(argument);
    }
    Json environment = Json::array();
    for (const launcher::EnvironmentOverride& override : request.environment) {
        environment.push_back(
            Json{{"name", override.name}, {"value", override.value ? Json(*override.value) : Json(nullptr)}});
    }
    return Json{{"argv", std::move(argv)},
                {"workingDirectory", pathText(launcher::workingDirectoryOf(request))},
                {"environment", std::move(environment)},
                {"hideConsoleWindow", request.hideConsoleWindow},
                {"waitStrategy", std::string(launcher::toString(request.waitStrategy))},
                {"label", request.label}};
}

Json dryRunJson(const DryRun& run) {
    const catalog::LaunchResolution& resolution = run.resolution;
    Json json = translationJson(resolution.translation);
    json["game"] = run.id ? Json(run.id->toString()) : Json(nullptr);
    json["requestedGame"] = run.requestedId ? Json(run.requestedId->toString()) : Json(nullptr);
    json["system"] = run.system->name.str();
    json["path"] = run.requestedPath;
    json["listedPath"] = run.game.relativePath;
    json["directory"] = run.game.directory;
    json["availability"] = std::string(catalog::toString(run.game.availability));
    json["romPath"] = pathText(run.romPath);
    json["title"] = run.game.title;
    json["alternativeEmulator"] = run.systemAlternativeEmulator.empty() ? Json(nullptr)
                                                                        : Json(run.systemAlternativeEmulator);
    json["chosen"] = chosenJson(resolution.chosen, run.origin);
    json["command"] = run.commandText.empty() ? Json(nullptr) : Json(run.commandText);
    json["ready"] = resolution.readiness() == catalog::LaunchReadiness::Ready;
    json["request"] = resolution.translation.request ? requestJson(*resolution.translation.request) : Json(nullptr);
    json["commandLine"] = run.commandLine ? Json(*run.commandLine) : Json(nullptr);
    json["guard"] = run.guard ? Json{{"rule", std::string(launcher::toString(run.guard->rule))},
                                     {"allowed", run.guard->allowed()},
                                     {"message", run.guard->message}}
                              : Json(nullptr);
    json["started"] = false;
    return json;
}

/// The lines a dry run and a launch share: the game, the choice, the readiness and the translation's details.
void writeResolution(const DryRun& run, Output& out) {
    const catalog::LaunchResolution& resolution = run.resolution;
    const auto line = [&](std::string_view label, const std::string& text) {
        out.writeLine(fmt::format("{:<20}{}", label, text));
    };
    std::string game = fmt::format("{} \"{}\" ({}{})", run.system->name.str(), run.game.relativePath,
                                   run.game.directory ? "folder, " : "", catalog::toString(run.game.availability));
    if (run.id) {
        game = fmt::format("{}: {}", run.id->toString(), game);
        if (run.requestedId && *run.requestedId != *run.id) {
            game += fmt::format(" (for {}, an alias)", run.requestedId->toString());
        }
    }
    line("Game", game);
    line("Title", run.game.title);
    line("ROM", pathText(run.romPath));
    line("Launches with",
         fmt::format("{} ({})", choiceText(resolution.chosen.choice), whyText(resolution.chosen, run.origin)));
    if (!run.commandText.empty()) {
        line("Command", run.commandText);
    }
    for (const std::string& warning : resolution.chosen.warnings) {
        line("Warning", warning);
    }
    line("Readiness", std::string(catalog::describe(resolution.readiness())));
    writeTranslationDetails(out, resolution.translation, "", 20, true);
}

void writeDryRunText(const DryRun& run, Output& out) {
    const catalog::LaunchResolution& resolution = run.resolution;
    const auto line = [&](std::string_view label, const std::string& text) {
        out.writeLine(fmt::format("{:<20}{}", label, text));
    };
    writeResolution(run, out);
    if (const auto& request = resolution.translation.request) {
        out.writeLine(resolution.readiness() == catalog::LaunchReadiness::Ready ? "Argv:"
                                                                                : "Argv (blocked, not runnable):");
        out.writeLine(fmt::format("  [0] {}", pathText(request->executable)));
        for (std::size_t i = 0; i < request->arguments.size(); ++i) {
            out.writeLine(fmt::format("  [{}] {}", i + 1, request->arguments[i]));
        }
        line("Working folder", pathText(launcher::workingDirectoryOf(*request)));
        if (request->environment.empty()) {
            line("Environment", "inherited, unchanged");
        } else {
            for (const launcher::EnvironmentOverride& override : request->environment) {
                line("Environment", override.value ? fmt::format("{}={}", override.name, *override.value)
                                                   : fmt::format("{} removed", override.name));
            }
        }
        line("Console window", request->hideConsoleWindow ? "hidden (%HIDEWINDOW%)" : "shown");
        if (run.commandLine) {
            line("Command line", *run.commandLine);
        }
    }
    if (run.guard) {
        switch (run.guard->rule) {
        case launcher::GuardRule::Inactive:
            line("Automation guard", "inactive (RV_LAUNCH_ALLOWLIST is not set)");
            break;
        case launcher::GuardRule::Allowed:
            line("Automation guard", "allowed (RV_LAUNCH_ALLOWLIST)");
            break;
        default:
            line("Automation guard", fmt::format("would refuse it ({}): {}", launcher::toString(run.guard->rule),
                                                 run.guard->message));
            break;
        }
    }
    out.writeLine("Dry run: nothing was started.");
}

// --- The game of the catalog -----------------------------------------------------------------------------------------

/// Resolves the launch of `game` right before it starts (or for a dry run): the system as the library configures it
/// now, the game's file on disk now, the game's <altemulator> and the system's <alternativeEmulator> as the last import
/// read them, Retroverse's choices from `store` (the game's, then the system's before emulators.json's).
Expected<DryRun> resolveCatalogGame(LaunchLibrary& library, catalog_db::SessionStore& store, const CatalogGame& game) {
    DryRun run;
    run.id = game.id;
    run.requestedId = game.requested;
    const catalog::CatalogEntry& entry = game.details.indexed.entry;
    run.requestedPath = entry.key.relativePath();
    run.game.relativePath = run.requestedPath;
    run.game.directory = entry.file && entry.file->directory;
    run.game.title = entry.title;
    if (game.details.system) {
        run.systemAlternativeEmulator = game.details.system->alternativeEmulator;
    }

    auto assigned = store.systemChoices(library.profile.id);
    if (!assigned) {
        return propagate(assigned.error());
    }
    library.assignedChoices.clear();
    for (catalog_db::SystemChoiceRow& row : *assigned) {
        library.assignedChoices.emplace(row.system.str(), std::move(row.stored.choice));
    }
    auto gameChoice = store.gameChoice(game.id);
    if (!gameChoice) {
        return propagate(gameChoice.error());
    }

    run.system = library.findSystem(entry.system.str());
    if (run.system == nullptr) {
        // The configuration no longer defines the system: nothing to translate.
        run.catalogSystem = game.details.system.value_or(catalog::SystemInfo{});
        run.catalogSystem->name = entry.system;
        run.system = &*run.catalogSystem;
        run.resolution.chosen.choice = gameChoice->has_value() ? (**gameChoice).choice : launcher::LaunchChoice{};
        run.resolution.chosen.source = gameChoice->has_value() ? catalog::ChoiceSource::RetroverseGame
                                                               : catalog::ChoiceSource::FirstCommand;
        run.resolution.translation.blockers.push_back(catalog::ReadinessBlocker{
            catalog::LaunchReadiness::RequiresConfiguration, "system-not-configured",
            fmt::format("the library no longer configures the system {} (es_systems.xml)", entry.system.str())});
        return run;
    }
    const catalog::SystemInfo& system = *run.system;
    if (system.romFolder.empty()) {
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = catalog::SourceUnavailableReason::FolderMissing;
    } else if (const Status found = findGame(system, run); !found) {
        // A stored path no file can have now: the game cannot be there.
        run.game.availability = catalog::Availability::Missing;
    }
    if (!system.romFolder.empty()) {
        run.romPath = (system.romFolder / core::pathFromUtf8(run.game.relativePath)).make_preferred();
    }
    catalog::LaunchChoices choices = systemChoices(library, system, run.systemAlternativeEmulator);
    choices.gameAltEmulator = entry.altEmulator;
    if (*gameChoice) {
        choices.game = (**gameChoice).choice;
        run.origin = std::string(kAssignedOrigin);
    } else {
        run.origin = std::string(systemChoiceOrigin(library, system.name.str()));
    }
    run.resolution = catalog::resolveLaunch(system, &run.game, choices, library.context());
    finishResolution(library, run);
    return run;
}

// --- The launch ------------------------------------------------------------------------------------------------------

/// Wakes the wait when the runner queues an event (StartCallbacks::wake, called from the runner's thread).
struct WakeSignal {
    std::mutex mutex;
    std::condition_variable condition;
    bool woken = false;

    void notify() {
        {
            const std::lock_guard lock(mutex);
            woken = true;
        }
        condition.notify_one();
    }
    void waitFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, timeout, [this] { return woken; });
        woken = false;
    }
};

/// Set by Ctrl+C or Ctrl+Break while a launch is waited for (StopOnInterrupt).
std::atomic<bool> g_stopWaiting{false};

BOOL WINAPI onConsoleControl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stopWaiting = true;
        return TRUE; // handled: the wait ends, records the session and returns; the game runs on
    }
    return FALSE;
}

/// While it lives, Ctrl+C and Ctrl+Break stop the wait instead of ending the process (the game, in a console of its own
/// or a window, never gets them).
class StopOnInterrupt {
public:
    StopOnInterrupt() noexcept {
        g_stopWaiting = false;
        m_installed = SetConsoleCtrlHandler(&onConsoleControl, TRUE) != FALSE;
    }
    ~StopOnInterrupt() {
        if (m_installed) {
            SetConsoleCtrlHandler(&onConsoleControl, FALSE);
        }
    }
    StopOnInterrupt(const StopOnInterrupt&) = delete;
    StopOnInterrupt& operator=(const StopOnInterrupt&) = delete;

    [[nodiscard]] static bool requested() noexcept { return g_stopWaiting.load(); }

private:
    bool m_installed = false;
};

/// How a launch is run.
struct LaunchOptions {
    bool wait = false;
    bool test = false;
    /// --wait-limit; zero: none.
    std::chrono::seconds waitLimit{0};
};

/// What a launch did.
struct LaunchReport {
    DryRun run;
    bool started = false;
    std::vector<catalog::PlaySession> interrupted;
    /// The session as recorded (or as it would have been, when it could not be).
    catalog::PlaySession session;
    bool sessionRecorded = false;
    std::optional<Error> sessionError;
    bool verified = false;
    std::optional<Error> verificationError;
};

/// The blockers' sentence of a blocked attempt: "<code>: <message>; ..." (the first three).
std::string blockersText(const std::vector<catalog::ReadinessBlocker>& blockers) {
    std::string text;
    for (std::size_t i = 0; i < blockers.size() && i < 3; ++i) {
        text += fmt::format("{}{}: {}", text.empty() ? "" : "; ", blockers[i].code, blockers[i].message);
    }
    if (blockers.size() > 3) {
        text += fmt::format("; and {} more", blockers.size() - 3);
    }
    return text;
}

/// Records `report.session` through `store` with the patience a session's end deserves.
void recordFinished(catalog_db::SessionRecorder& recorder, LaunchReport& report) {
    recorder.update(report.session);
    if (auto finished = recorder.finish(catalog_db::kPatientRetry); !finished) {
        report.sessionError = finished.error();
        spdlog::error("The play session of {} could not be recorded: {}", report.session.game.toString(),
                      finished.error().toString());
    } else {
        report.sessionRecorded = true;
    }
    report.session.id = recorder.session().id;
}

/// Starts the resolved, ready launch of `report.run` and follows it as `options` say, recording its session.
void startAndFollow(CommandContext& context, catalog_db::SessionStore& store, const launcher::LaunchRequest& request,
                    const LaunchOptions& options, LaunchReport& report) {
    catalog::PlaySession& session = report.session;
    session.outcome = catalog::PlaySessionOutcome::Running;
    // The wall-clock start and the monotonic one, taken together: the end's wall-clock time is derived from both.
    session.started = std::chrono::system_clock::now();
    const launcher::LaunchClock::time_point begun = launcher::LaunchClock::now();
    catalog_db::SessionRecorder recorder(store, session);
    // One attempt (the writer's short busy timeout): a busy database never holds the game back for long.
    if (auto recorded = recorder.begin(); !recorded) {
        spdlog::warn("The start of the play session of {} is not recorded yet: {}", session.game.toString(),
                     recorded.error().toString());
    }

    platform_win::Win32ProcessRunner runner;
    launcher::LaunchSession launch(request, runner);
    const auto wake = std::make_shared<WakeSignal>();
    launch.start(begun, launcher::StartCallbacks{[wake] { wake->notify(); }, {}});
    report.started = launch.process() != nullptr;
    if (report.started) {
        // The running row names the started process (one quick attempt again; a busy database keeps it pending).
        session.processId = launch.processId();
        recorder.update(session);
        if (auto flushed = recorder.flush(); !flushed) {
            spdlog::warn("The play session of {} is not recorded yet: {}", session.game.toString(),
                         flushed.error().toString());
        }
        context.err.writeLine(fmt::format(
            "{}: {} started (process {}){}", kName, request.label, launch.processId(),
            options.wait ? ": waiting for it to end (Ctrl+C stops waiting; the game runs on)" : ""));
    }

    std::optional<StopOnInterrupt> interrupt;
    if (options.wait) {
        interrupt.emplace();
    }
    catalog::LeftRunningReason reason = catalog::LeftRunningReason::None;
    // The owner stopped following a session that waited for the user's confirmation (UserConfirmReturn): its end is
    // that confirmation, which never came, so the game is left running whatever the tree says.
    bool stoppedAwaiting = false;
    const auto stop = [&](catalog::LeftRunningReason why, launcher::LaunchClock::time_point now) {
        reason = why;
        stoppedAwaiting = why != catalog::LeftRunningReason::None &&
                          launch.state() == launcher::SessionState::AwaitingConfirmation;
        launch.confirmReturn(now); // never a kill: the session stops following the processes
    };
    auto lastFlush = std::chrono::steady_clock::now();
    bool toldStillRunning = false;
    bool toldConfirm = false;
    while (!launch.finished()) {
        wake->waitFor(kWaitSlice);
        const launcher::LaunchClock::time_point now = launcher::LaunchClock::now();
        launch.poll(now);
        if (launch.finished()) {
            break;
        }
        const launcher::SessionTimes& times = launch.times();
        if (options.wait && StopOnInterrupt::requested()) {
            // Ctrl+C: the user's confirmation that the game is over when the session asks for it, else stop waiting.
            stop(launch.state() == launcher::SessionState::AwaitingConfirmation ? catalog::LeftRunningReason::None
                                                                                : catalog::LeftRunningReason::Quit,
                 now);
            break;
        }
        if (!options.wait &&
            now >= times.started.value_or(begun) + request.timeouts.startupGrace + kGraceMargin) {
            stop(catalog::LeftRunningReason::NotWaited, now);
            break;
        }
        if (options.waitLimit.count() > 0 && now - begun >= options.waitLimit) {
            stop(catalog::LeftRunningReason::WaitLimit, now);
            break;
        }
        if (launch.state() == launcher::SessionState::StillRunning && !toldStillRunning) {
            toldStillRunning = true;
            context.err.writeLine(fmt::format("{}: no window of {} for {} s; still waiting (Ctrl+C stops waiting, the "
                                              "game runs on)",
                                              kName, request.label,
                                              std::chrono::duration_cast<std::chrono::seconds>(
                                                  request.timeouts.windowTimeout)
                                                  .count()));
        }
        if (launch.state() == launcher::SessionState::AwaitingConfirmation && !toldConfirm) {
            toldConfirm = true;
            context.err.writeLine(fmt::format("{}: {} exited; its emulator may still run. Press Ctrl+C once you have "
                                              "finished playing.",
                                              kName, request.label));
        }
        if (recorder.pending() && std::chrono::steady_clock::now() - lastFlush >= kPendingRetryInterval) {
            lastFlush = std::chrono::steady_clock::now();
            if (auto flushed = recorder.flush(); !flushed) {
                spdlog::warn("The play session of {} is not recorded yet: {}", session.game.toString(),
                             flushed.error().toString());
            }
        }
    }

    const launcher::LaunchOutcome& outcome = launch.outcome();
    const launcher::SessionTimes& times = launch.times();
    const launcher::LaunchClock::time_point now = launcher::LaunchClock::now();
    // The authoritative exit when the game ended by itself, else the moment the session ended (the owner's return).
    const launcher::LaunchClock::time_point end =
        !outcome.confirmedByUser && times.exited ? *times.exited : times.ended.value_or(now);
    recordOutcome(session, outcome,
                  session.started + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                        std::max(end - begun, launcher::LaunchClock::duration::zero())),
                  reason);
    if (stoppedAwaiting && session.outcome == catalog::PlaySessionOutcome::Returned) {
        session.outcome = catalog::PlaySessionOutcome::LeftRunning;
        session.leftRunning = reason;
        session.quickExit = false; // a launcher stub's quick exit is no hint while the user's word is awaited
    }
    if (times.started) {
        session.runTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(end - *times.started, launcher::LaunchClock::duration::zero()));
    }
    session.processId = launch.processId();
    session.refusedByGuard =
        outcome.failure == launcher::FailureReason::StartFailed &&
        (outcome.refusedByGuard || (report.run.guard && !report.run.guard->allowed()));
    recordFinished(recorder, report);
}

/// Records the launch verification of a session that returned normally.
void verifyIfReturnedNormally(catalog_db::SessionStore& store, const LaunchLibrary& library,
                              const launcher::LaunchRequest& request, LaunchReport& report) {
    if (!report.sessionRecorded || !catalog::returnedNormally(report.session)) {
        return;
    }
    catalog_db::LaunchVerification verification;
    verification.library = library.profile.id;
    verification.system = report.run.system->name;
    verification.choice = report.session.choice;
    verification.game = report.session.game;
    verification.session = report.session.id;
    verification.test = report.session.test;
    verification.program = request.executable;
    verification.verifiedAt = report.session.ended.value_or(std::chrono::system_clock::now());
    if (auto recorded = store.recordVerification(verification, catalog_db::kPatientRetry); !recorded) {
        report.verificationError = recorded.error();
        spdlog::error("The launch verification of {} for {} could not be recorded: {}",
                      choiceText(verification.choice), verification.system.str(), recorded.error().toString());
        return;
    }
    report.verified = true;
}

Json launchJson(const LaunchReport& report, const LaunchOptions& options) {
    Json json = dryRunJson(report.run);
    json["started"] = report.started;
    json["waited"] = options.wait;
    json["test"] = options.test;
    json["session"] = sessionJson(report.session);
    json["sessionRecorded"] = report.sessionRecorded;
    json["sessionError"] = report.sessionError ? Json(report.sessionError->toString()) : Json(nullptr);
    json["verified"] = report.verified;
    json["verificationError"] = report.verificationError ? Json(report.verificationError->toString()) : Json(nullptr);
    Json interrupted = Json::array();
    for (const catalog::PlaySession& session : report.interrupted) {
        interrupted.push_back(sessionJson(session));
    }
    json["interrupted"] = std::move(interrupted);
    return json;
}

void writeLaunchText(const LaunchReport& report, Output& out) {
    const auto line = [&](std::string_view label, const std::string& text) {
        out.writeLine(fmt::format("{:<20}{}", label, text));
    };
    for (const catalog::PlaySession& session : report.interrupted) {
        line("Interrupted", fmt::format("session {} of {}, left open by process {} (gone), closed as interrupted",
                                        session.id, session.game.toString(), session.owner.processId));
    }
    writeResolution(report.run, out);
    const catalog::PlaySession& session = report.session;
    if (session.outcome == catalog::PlaySessionOutcome::Blocked) {
        line("Not started", fmt::format("the game is not ready: {}",
                                        catalog::describe(report.run.resolution.readiness())));
    } else {
        line("Started", report.started ? fmt::format("{}, process {}", core::formatUtcIso8601(session.started),
                                                     session.processId)
                                       : std::string("no"));
        line("Outcome", outcomeText(session));
        if (!session.message.empty()) {
            line("Message", session.message);
        }
        if (session.runTime) {
            line("Ran for", durationText(*session.runTime));
        }
    }
    if (report.sessionRecorded) {
        line("Session", fmt::format("{} (recorded{})", session.id, session.test ? ", a launch test" : ""));
    } else {
        line("Session", fmt::format("NOT RECORDED: {}", report.sessionError ? report.sessionError->toString()
                                                                            : std::string("unknown error")));
    }
    if (report.verified) {
        line("Verified", fmt::format("{} for {}", choiceText(session.choice), report.run.system->name.str()));
    } else if (session.test) {
        line("Verified", report.verificationError
                             ? fmt::format("NOT RECORDED: {}", report.verificationError->toString())
                             : std::string("no: the game did not return normally (a clean exit that is no quick "
                                           "exit, nothing left running)"));
    }
}

/// The exit code of a launch (see the header comment).
ExitCode launchExitCode(const LaunchReport& report, const LaunchOptions& options) {
    if (!report.sessionRecorded || (options.test && report.verificationError)) {
        return ExitCode::RuntimeFailure;
    }
    if (options.test) {
        return report.verified ? ExitCode::Ok : ExitCode::ProblemsFound;
    }
    using catalog::PlaySessionOutcome;
    switch (report.session.outcome) {
    case PlaySessionOutcome::Returned:
        return report.session.quickExit && !report.session.confirmedByUser ? ExitCode::ProblemsFound : ExitCode::Ok;
    case PlaySessionOutcome::LeftRunning:
        return ExitCode::Ok;
    default:
        return ExitCode::ProblemsFound;
    }
}

/// `launch --game <id>`: the launch itself.
ExitCode launchCatalogGame(CommandContext& context, catalog::GameId requested, const LaunchOptions& options,
                           bool json, std::optional<Output>& output) {
    ExitCode failure = ExitCode::Ok;
    const auto library = loadLaunchLibrary(context, kName, failure);
    if (!library) {
        return failure;
    }
    auto catalog = openLaunchCatalog(context.paths);
    if (!catalog) {
        return catalogFailure(context, catalog.error());
    }
    LaunchReport report;
    report.interrupted = recoverInterruptedSessions(*catalog->store);
    const auto game = readCatalogGame(context, *catalog->query, *catalog->store, requested, library->profile.id,
                                      failure);
    if (!game) {
        return failure;
    }
    auto resolved = resolveCatalogGame(*library, *catalog->store, *game);
    if (!resolved) {
        return catalogFailure(context, resolved.error());
    }
    report.run = std::move(*resolved);
    const catalog::LaunchResolution& resolution = report.run.resolution;
    spdlog::info("launch --game {}: {} {}: {}", game->id.toString(), report.run.system->name.str(),
                 choiceText(resolution.chosen.choice), catalog::toString(resolution.readiness()));
    if (report.run.commandLine) {
        spdlog::debug("launch --game {}: command line {}", game->id.toString(), *report.run.commandLine);
    }

    catalog::PlaySession& session = report.session;
    session.game = game->id;
    session.choice = resolution.chosen.choice;
    session.choiceSource = resolution.chosen.source;
    session.test = options.test;
    session.owner = currentSessionOwner();
    const std::optional<launcher::LaunchRequest> request = resolution.request();
    if (!request) {
        // Not ready: nothing starts; the attempt is recorded.
        session.outcome = catalog::PlaySessionOutcome::Blocked;
        session.started = std::chrono::system_clock::now();
        session.ended = session.started;
        session.readiness = resolution.readiness();
        session.message = blockersText(resolution.translation.blockers);
        catalog_db::SessionRecorder recorder(*catalog->store, session);
        recordFinished(recorder, report);
    } else {
        startAndFollow(context, *catalog->store, *request, options, report);
        verifyIfReturnedNormally(*catalog->store, *library, *request, report);
    }
    spdlog::info("launch --game {}: session {}: {}", game->id.toString(), report.session.id,
                 outcomeText(report.session));

    Output& out = output ? *output : context.out;
    if (json) {
        out.write(launchJson(report, options).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    } else {
        writeLaunchText(report, out);
    }
    if (!finishOutput(context, output, kName)) {
        return ExitCode::RuntimeFailure;
    }
    return launchExitCode(report, options);
}

/// `launch --dry-run --game <id>`: the catalog read only, nothing written.
ExitCode dryRunCatalogGame(CommandContext& context, catalog::GameId requested, bool json,
                           std::optional<Output>& output) {
    ExitCode failure = ExitCode::Ok;
    const auto library = loadLaunchLibrary(context, kName, failure);
    if (!library) {
        return failure;
    }
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    auto store = catalog_db::SessionStore::open(reader->query->connection());
    if (!store) {
        return catalogFailure(context, store.error());
    }
    const auto game = readCatalogGame(context, *reader->query, **store, requested, library->profile.id, failure);
    if (!game) {
        return failure;
    }
    auto run = resolveCatalogGame(*library, **store, *game);
    if (!run) {
        return catalogFailure(context, run.error());
    }
    const catalog::LaunchReadiness readiness = run->resolution.readiness();
    spdlog::info("launch --dry-run --game {}: {}", game->id.toString(), catalog::toString(readiness));
    if (run->commandLine) {
        spdlog::debug("launch --dry-run --game {}: command line {}", game->id.toString(), *run->commandLine);
    }
    Output& out = output ? *output : context.out;
    if (json) {
        out.write(dryRunJson(*run).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    } else {
        writeDryRunText(*run, out);
    }
    if (!finishOutput(context, output, kName)) {
        return ExitCode::RuntimeFailure;
    }
    return readiness == catalog::LaunchReadiness::Ready ? ExitCode::Ok : ExitCode::ProblemsFound;
}

/// `launch --dry-run --system <name> --path <path>` (I2 step 10).
ExitCode dryRunPath(CommandContext& context, const std::string& systemName, const std::string& pathArgument,
                    bool json, std::optional<Output>& output) {
    ExitCode failure = ExitCode::Ok;
    const auto library = loadLaunchLibrary(context, kName, failure);
    if (!library) {
        return failure;
    }
    DryRun run;
    run.system = library->findSystem(systemName);
    if (run.system == nullptr) {
        context.err.writeLine(fmt::format("{}: the library configures no system named \"{}\"", kName, systemName));
        return ExitCode::UsageError;
    }
    const catalog::SystemInfo& system = *run.system;
    if (system.romFolder.empty()) {
        context.err.writeLine(
            fmt::format("{}: the ROM folder of {} cannot be resolved (see the issues of 'retroverse-cli discover')",
                        kName, systemName));
        return ExitCode::UsageError;
    }
    const auto relative = core::resolveRelativePath(pathArgument, system.romFolder);
    if (!relative) {
        context.err.writeLine(fmt::format("{}: --path: {}", kName, relative.error().toString()));
        return ExitCode::UsageError;
    }
    run.requestedPath = *relative;
    if (const Status found = findGame(system, run); !found) {
        context.err.writeLine(fmt::format("{}: --path: {}", kName, found.error().toString()));
        return ExitCode::UsageError;
    }
    // Retroverse's choice for the system: the catalog database's (assign) before emulators.json's.
    loadAssignedChoices(*library, context.paths);
    catalog::LaunchChoices choices = systemChoices(*library, system, {});
    run.origin = std::string(systemChoiceOrigin(*library, systemName));
    readGameEntry(*library, system, run, choices);
    run.romPath = (system.romFolder / core::pathFromUtf8(run.game.relativePath)).make_preferred();
    run.resolution = catalog::resolveLaunch(system, &run.game, choices, library->context());
    finishResolution(*library, run);
    const catalog::LaunchReadiness readiness = run.resolution.readiness();
    spdlog::info("launch --dry-run: {}: {}", systemName, catalog::toString(readiness));
    if (run.commandLine) {
        spdlog::debug("launch --dry-run: command line {}", *run.commandLine);
    }

    Output& out = output ? *output : context.out;
    if (json) {
        out.write(dryRunJson(run).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    } else {
        writeDryRunText(run, out);
    }
    if (!finishOutput(context, output, kName)) {
        return ExitCode::RuntimeFailure;
    }
    return readiness == catalog::LaunchReadiness::Ready ? ExitCode::Ok : ExitCode::ProblemsFound;
}

} // namespace

ExitCode runLaunch(CommandContext& context) {
    core::ArgParser parser(
        std::string(kName),
        "Launches a game of the catalog in its emulator (--game), resolved right before the launch, and records the "
        "attempt as a play session; nothing is ever terminated. Without --wait the game is followed through its "
        "startup grace and left running; --wait follows it to its end (Ctrl+C stops waiting); --test waits and "
        "records a launch verification when the game returns normally. --dry-run resolves the launch of --game, or "
        "of the game at --path in the ROM folder of --system, and prints what would run, without starting anything.");
    parser.option("game", "id", "The game of the catalog: its GameId (g and 16 hexadecimal digits).")
        .flag("wait", "Wait for the game to end and record its outcome (Ctrl+C stops waiting; the game runs on).")
        .option("wait-limit", "seconds", "With --wait or --test: stop waiting after this long (the game runs on).")
        .flag("test", "A launch verification: wait, and record it for the choice and the system when the game "
                      "returns normally.")
        .flag("dry-run", "Resolve the launch and print it; start nothing.")
        .option("system", "name", "With --dry-run: the game's system (an ES-DE system name).")
        .option("path", "path", "With --dry-run: the game's path relative to the system's ROM folder.")
        .flag("json", "Print the results as JSON.");
    addOutputOption(parser);
    const auto args = parser.parse(context.arguments);
    if (!args) {
        context.err.writeLine(fmt::format("{}: {}", kName, args.error().toString()));
        return ExitCode::UsageError;
    }
    if (args->has("help")) {
        context.out.write(parser.helpText());
        return ExitCode::Ok;
    }
    const auto usage = [&](std::string_view message) {
        context.err.writeLine(fmt::format("{}: {}", kName, message));
        return ExitCode::UsageError;
    };
    auto output = outputFileOption(*args);
    if (!output) {
        return usage(output.error().toString());
    }
    const bool dryRun = args->has("dry-run");
    const bool json = args->has("json");
    const bool hasPath = args->has("system") || args->has("path");
    LaunchOptions options;
    options.test = args->has("test");
    options.wait = args->has("wait") || options.test;
    if (args->has("wait-limit")) {
        const auto limit = args->intValue("wait-limit", 0);
        if (!limit || *limit < 1 || *limit > kMaxWaitLimitSeconds) {
            return usage(fmt::format("--wait-limit takes a number of seconds from 1 to {}", kMaxWaitLimitSeconds));
        }
        if (!options.wait) {
            return usage("--wait-limit needs --wait or --test");
        }
        options.waitLimit = std::chrono::seconds(*limit);
    }

    std::optional<catalog::GameId> game;
    if (args->has("game")) {
        const auto id = catalog::GameId::parse(args->value("game").value_or(""));
        if (!id) {
            return usage(fmt::format("--game: {}", id.error().toString()));
        }
        if (hasPath) {
            return usage("--game takes neither --system nor --path");
        }
        game = *id;
    }
    if (dryRun) {
        if (options.wait || args->has("wait-limit")) {
            return usage("--dry-run starts nothing: it takes neither --wait, --wait-limit nor --test");
        }
        if (game) {
            return dryRunCatalogGame(context, *game, json, *output);
        }
        const std::string systemName = args->value("system").value_or("");
        const std::string pathArgument = args->value("path").value_or("");
        if (systemName.empty() || pathArgument.empty()) {
            return usage("--dry-run needs --game <id>, or --system <name> and --path <relative path>");
        }
        return dryRunPath(context, systemName, pathArgument, json, *output);
    }
    if (hasPath) {
        return usage("--system and --path name a game for --dry-run only: launch a game of the catalog with --game "
                     "<id> (its GameId: 'retroverse-cli games', 'search')");
    }
    if (!game) {
        return usage("launch needs --game <id> (its GameId: 'retroverse-cli games', 'search'), or --dry-run");
    }
    return launchCatalogGame(context, *game, options, json, *output);
}

} // namespace rv::cli
