#include <rv/app/app_catalog.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog_db/database.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/launcher/launch_plan.hpp>
#include <rv/ui/ui_assets.hpp>

namespace rv::app {
namespace {

namespace lo = launch_outcome;
using launcher::LaunchClock;

/// FILETIME ticks between 1601-01-01 and 1970-01-01, the system clock's epoch.
constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ull;

/// The blockers' sentence of a blocked attempt: "<code>: <message>; ..." (the first three), as retroverse-cli records
/// it.
std::string blockersText(const std::vector<catalog::ReadinessBlocker>& blockers) {
    std::string text;
    for (std::size_t index = 0; index < blockers.size() && index < 3; ++index) {
        text += fmt::format("{}{}: {}", text.empty() ? "" : "; ", blockers[index].code, blockers[index].message);
    }
    if (blockers.size() > 3) {
        text += fmt::format("; and {} more", blockers.size() - 3);
    }
    return text;
}

/// What a launch's choice is called in the logs and the report (LaunchInfo::profile): a profile's id, a command's
/// label ("" for a single unlabelled command).
std::string choiceName(const launcher::LaunchChoice& choice) {
    return choice.value;
}

} // namespace

std::uint64_t fileTimeTicksOf(std::chrono::system_clock::time_point time) noexcept {
    const auto ticks = std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
                           time.time_since_epoch())
                           .count();
    return static_cast<std::uint64_t>(ticks) + kUnixEpochTicks;
}

// --- Sessions ----------------------------------------------------------------------------------------------------

catalog::PlaySession startedPlaySession(catalog::PlaySession session, const launcher::LaunchSession& launch,
                                        std::chrono::system_clock::time_point wallNow, LaunchClock::time_point now) {
    session.outcome = catalog::PlaySessionOutcome::Running;
    const LaunchClock::time_point begun = launch.times().begun.value_or(now);
    session.started =
        wallNow - std::chrono::duration_cast<std::chrono::system_clock::duration>(std::max(now - begun,
                                                                                            LaunchClock::duration::zero()));
    session.processId = launch.processId();
    return session;
}

catalog::PlaySession finishedPlaySession(catalog::PlaySession running, const launcher::LaunchSession& launch,
                                         LaunchEndCause cause, bool stoppedAwaiting, bool guardAllowed,
                                         LaunchClock::time_point now) {
    catalog::PlaySession session = std::move(running);
    const launcher::LaunchOutcome& outcome = launch.outcome();
    const launcher::SessionTimes& times = launch.times();
    const LaunchClock::time_point begun = times.begun.value_or(now);
    // The authoritative exit when the game ended by itself, else the moment the session ended (the owner's return).
    const LaunchClock::time_point end =
        !outcome.confirmedByUser && times.exited ? *times.exited : times.ended.value_or(now);
    catalog::LeftRunningReason reason = catalog::LeftRunningReason::None;
    if (cause == LaunchEndCause::Quit) {
        reason = catalog::LeftRunningReason::Quit;
    } else if (cause == LaunchEndCause::WaitLimit) {
        reason = catalog::LeftRunningReason::WaitLimit;
    }
    catalog::recordOutcome(session, outcome,
                           session.started + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                 std::max(end - begun, LaunchClock::duration::zero())),
                           reason);
    if (stoppedAwaiting && session.outcome == catalog::PlaySessionOutcome::Returned) {
        // Its end was the user's confirmation, which never came: the game is left running whatever the tree says.
        session.outcome = catalog::PlaySessionOutcome::LeftRunning;
        session.leftRunning = reason;
        session.quickExit = false;
    }
    if (times.started) {
        session.runTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(end - *times.started, LaunchClock::duration::zero()));
    }
    session.processId = launch.processId();
    session.refusedByGuard =
        outcome.failure == launcher::FailureReason::StartFailed && (!guardAllowed || outcome.refusedByGuard);
    return session;
}

catalog::PlaySession blockedPlaySession(catalog::PlaySession session, const catalog::LaunchResolution& resolution,
                                        std::chrono::system_clock::time_point wallNow) {
    session.outcome = catalog::PlaySessionOutcome::Blocked;
    session.started = wallNow;
    session.ended = wallNow;
    session.readiness = resolution.readiness();
    session.message = blockersText(resolution.translation.blockers);
    return session;
}

LaunchMessage refusalOf(const ResolvedLaunch& resolved) {
    if (resolved.error) {
        return LaunchMessage{std::string(lo::kNotReady), "unresolved", resolved.error->message};
    }
    LaunchMessage message{std::string(lo::kNotReady), std::string(catalog::toString(resolved.resolution.readiness())),
                          {}};
    for (const catalog::ReadinessBlocker& blocker : resolved.resolution.translation.blockers) {
        if (blocker.readiness == resolved.resolution.readiness()) {
            message.detail = blocker.message;
            break;
        }
    }
    return message;
}

void recordRefresh(RefreshRecord& record, const RefreshSummary& summary, const std::optional<TimeSummary>& lists) {
    record.outcome = std::string(toString(summary.outcome));
    record.error = summary.error ? summary.error->toString() : std::string();
    record.ms = summary.milliseconds;
    record.entries = summary.entries;
    record.added = summary.added;
    record.removed = summary.removed;
    record.changed = summary.changed;
    record.moved = summary.moved;
    record.reviews = summary.reviews;
    record.guarded = summary.guarded;
    record.slices = summary.slicing.boundaries;
    record.served = summary.slicing.served;
    record.longestSliceMs = summary.slicing.longestSliceMs;
    record.lists = lists;
}

// --- The mock catalog ----------------------------------------------------------------------------------------------

MockAppCatalog::MockAppCatalog(MockCatalog catalog, launcher::EmulatorProfiles baseProfiles)
    : m_catalog(std::move(catalog)), m_profiles(mockCatalogProfiles(std::move(baseProfiles), m_catalog)),
      m_source(m_catalog.entries()) {
    for (const launcher::ProfileIssue& issue : m_catalog.profiles.issues) {
        spdlog::warn("Mock catalog profiles: {}", issue.toString());
    }
}

bool MockAppCatalog::requestLaunch(std::uint64_t token, const ui::CatalogEntry& entry) {
    if (m_decision) {
        return false;
    }
    LaunchDecision decision;
    decision.token = token;
    decision.info = LaunchInfo{entry.id, entry.title, {}};
    const MockGame* game = m_catalog.find(entry.id);
    if (game == nullptr || !game->launch) {
        decision.refusal = LaunchMessage{std::string(lo::kNotConfigured), std::string(lo::kReasonNoLaunch), {}};
        m_decision = std::move(decision);
        return true;
    }
    const MockLaunch& target = *game->launch;
    decision.info.profile = target.profile;
    const launcher::EmulatorProfile* profile = m_profiles.find(target.profile);
    if (profile == nullptr) {
        decision.refusal =
            LaunchMessage{std::string(lo::kNotConfigured), std::string(lo::kReasonUnknownProfile), target.profile};
        m_decision = std::move(decision);
        return true;
    }
    launcher::LaunchPlan plan = launcher::planLaunch(target.target, *profile, m_probe);
    if (!plan.ready()) {
        spdlog::warn("Launch of '{}' blocked ({}): {}", entry.title, launcher::toString(plan.blocker->kind),
                     plan.blocker->message);
        decision.refusal = messageForBlocker(*plan.blocker, target.target, profile->displayName);
        m_decision = std::move(decision);
        return true;
    }
    decision.request = std::move(*plan.request);
    m_decision = std::move(decision);
    return true;
}

void MockAppCatalog::record(CatalogRecord& record) const {
    record.source = "mock";
    record.file = m_catalog.file;
    record.games = m_source.entries().size();
}

// --- The wizard's catalog to come -----------------------------------------------------------------------------------

PendingAppCatalog::PendingAppCatalog() : m_source(std::vector<ui::CatalogEntry>{}) {}

std::string_view PendingAppCatalog::notice() const noexcept {
    return ui::quick_catalog::kNoticeNoLibrary;
}

void PendingAppCatalog::record(CatalogRecord& record) const {
    record.source = "esde";
    record.notice = std::string(notice());
}

// --- The ES-DE catalog ---------------------------------------------------------------------------------------------

namespace {

CatalogWorker::Options workerOptions(const EsdeAppCatalog::Options& options) {
    CatalogWorker::Options worker;
    worker.paths = options.paths;
    worker.wake = options.wake;
    worker.owner = options.owner;
    worker.createDatabase = options.createDatabase;
    worker.otherImportsInterval = options.otherImportsInterval;
    if (options.adjust) {
        options.adjust(worker);
    }
    return worker;
}

} // namespace

EsdeAppCatalog::EsdeAppCatalog(Options options)
    : m_options(std::move(options)), m_worker(workerOptions(m_options)), m_source(m_worker) {}

EsdeAppCatalog::~EsdeAppCatalog() {
    shutdown();
}

double EsdeAppCatalog::sinceProcessStart(std::chrono::system_clock::time_point at) const {
    const std::uint64_t now = fileTimeTicksOf(at);
    if (m_options.processCreated == 0 || now < m_options.processCreated) {
        return 0.0;
    }
    return static_cast<double>(now - m_options.processCreated) / 10'000.0;
}

std::string_view EsdeAppCatalog::notice() const noexcept {
    if (m_source.state() == CatalogState::Ready && !m_source.listed()) {
        return ui::quick_catalog::kNoticeOpening;
    }
    return noticeOf(m_source.state());
}

bool EsdeAppCatalog::requestLaunchTest(std::uint64_t token, const ui::CatalogEntry& entry,
                                       const LaunchTestChoice& test) {
    const auto game = catalog::GameId::parse(entry.id);
    if (m_resolving || m_decision || !game || m_stopped || m_source.ended()) {
        return false;
    }
    m_resolving = token;
    Launch& launch = m_launches[token];
    launch.title = entry.title;
    launch.session.game = *game;
    launch.test = true;
    m_worker.resolve(token, *game, test);
    spdlog::info("Launch test of '{}' ({}) asked: resolving it on the catalog worker", entry.title, entry.id);
    return true;
}

void EsdeAppCatalog::launchDropped(std::uint64_t token) {
    const auto found = m_launches.find(token);
    if (found != m_launches.end() && found->second.test) {
        found->second.test = false;
        m_worker.forgetTest(token);
    }
}

bool EsdeAppCatalog::requestEmulatorTable() {
    if (m_stopped || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    m_tableAsked = true;
    m_table.reset();
    m_worker.emulatorTable(++m_emulatorSerial);
    return true;
}

bool EsdeAppCatalog::requestSampleGames(const catalog::SystemName& system, std::size_t limit) {
    if (m_stopped || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    m_samplesAsked = true;
    m_samples.reset();
    m_worker.sampleGames(++m_emulatorSerial, system, limit);
    return true;
}

bool EsdeAppCatalog::requestChoiceChange(ChoiceChange change) {
    if (m_stopped || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    ++m_changesAsked;
    m_worker.changeChoice(++m_emulatorSerial, std::move(change));
    return true;
}

bool EsdeAppCatalog::requestStatistics(std::size_t limit) {
    if (m_stopped || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    m_statisticsAsked = true;
    m_statistics.reset();
    m_worker.statistics(++m_statisticsSerial, limit);
    return true;
}

bool EsdeAppCatalog::requestGameControls(const catalog::GameId& game) {
    if (m_stopped || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    m_controlsAsked = 1;
    m_gameControls.reset();
    m_worker.gameControls(++m_controlsSerial, game);
    return true;
}

bool EsdeAppCatalog::requestGameStatus(const catalog::GameId& game, catalog::GameStatus status) {
    if (m_stopped || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    m_controlsAsked = 1;
    m_gameControls.reset();
    m_worker.setGameStatus(++m_controlsSerial, game, status);
    return true;
}

std::optional<std::filesystem::path> EsdeAppCatalog::programStartFolder() const {
    const auto profile = esde::readLibraryFile(m_options.paths.configDir() / esde::kLibraryFileName);
    return profile ? profile->installFolder : std::nullopt;
}

std::optional<ChoiceChanged> EsdeAppCatalog::takeChoiceChanged() {
    if (m_changes.empty()) {
        return std::nullopt;
    }
    ChoiceChanged changed = std::move(m_changes.front());
    m_changes.erase(m_changes.begin());
    return changed;
}

bool EsdeAppCatalog::requestLaunch(std::uint64_t token, const ui::CatalogEntry& entry) {
    // One decision at a time, until it is taken: a second Avvia would ask again with the same token.
    if (m_resolving || m_decision) {
        return false;
    }
    const auto game = catalog::GameId::parse(entry.id);
    if (!game) {
        LaunchDecision decision;
        decision.token = token;
        decision.info = LaunchInfo{entry.id, entry.title, {}};
        decision.refusal = LaunchMessage{std::string(lo::kNotReady), "unresolved", game.error().message};
        m_decision = std::move(decision);
        return true;
    }
    if (m_source.ended()) {
        LaunchDecision decision;
        decision.token = token;
        decision.info = LaunchInfo{entry.id, entry.title, {}};
        decision.refusal = LaunchMessage{std::string(lo::kNotReady), "unresolved", "the catalog worker ended"};
        m_decision = std::move(decision);
        return true;
    }
    m_resolving = token;
    Launch& launch = m_launches[token];
    launch.title = entry.title;
    launch.session.game = *game;
    m_worker.resolve(token, *game);
    spdlog::info("Launch of '{}' ({}) asked: resolving it on the catalog worker", entry.title, entry.id);
    return true;
}

CatalogChanges EsdeAppCatalog::poll() {
    CatalogResults results = m_worker.take();
    if (results.empty()) {
        return {};
    }
    // NFR-01's times are the worker's: when it was done, whenever the main thread looks.
    if (results.opened && results.opened->state == CatalogState::Ready && !m_openedMs) {
        m_openedMs = sinceProcessStart(results.opened->at);
    }
    if (results.opened && results.opened->firstList && !m_firstListMs) {
        m_firstListMs = sinceProcessStart(results.opened->firstList->at);
    }
    if (results.list && !results.list->error && !m_firstListMs) {
        m_firstListMs = sinceProcessStart(results.list->at);
    }
    if (results.list && !results.list->error && m_refreshRunning) {
        m_refreshLists.push_back(results.list->waitedMs); // a list answered while the refresh ran
    }
    if (results.list && !results.list->error && m_listWaits.size() < kMaxListWaits) {
        m_listWaits.push_back(results.list->waitedMs);
    }
    if (results.opened && results.opened->world) {
        m_worldReader = std::move(results.opened->world); // the building job's reader (I4 step 5a)
    }
    if (results.watchMap) {
        m_watchMap = std::move(results.watchMap); // I7 step 7: the file watcher's roots
        results.watchMap.reset();
    }
    // Another process's import (I7 step 7): its systems resolved again come as a refresh's, unless one of ours runs,
    // whose end resolves them again anyway.
    bool otherImport = false;
    if (results.otherImport) {
        if (!results.refreshed && !m_refreshRunning) {
            results.refreshed = std::move(results.otherImport);
            otherImport = true;
            ++m_otherImports;
        }
        results.otherImport.reset();
    }
    const CatalogDatabaseSource::Collected collected = m_source.collect(results);
    const bool refreshEnded = collectRefresh(results, m_source.ended());
    for (ResolvedLaunch& resolved : results.launches) {
        if (!m_resolving || *m_resolving != resolved.token) {
            continue; // a decision nobody waits for any more
        }
        m_resolving.reset();
        Launch& launch = m_launches[resolved.token];
        if (launch.begun) {
            continue; // never over a session that began (its started time and process)
        }
        LaunchDecision decision;
        decision.token = resolved.token;
        // The identity the GameId asked for stands for (an alias resolved), once resolved.
        decision.info.gameId = resolved.error ? resolved.requested.toString() : resolved.session.game.toString();
        decision.info.title = resolved.title.empty() ? launch.title : resolved.title;
        decision.info.profile = choiceName(resolved.resolution.chosen.choice);
        if (!resolved.error) {
            launch.session = resolved.session;
            launch.system = resolved.system;
            launch.guardAllowed = resolved.guardAllowed;
            if (auto request = resolved.resolution.request()) {
                decision.request = std::move(*request);
            } else {
                decision.refusal = refusalOf(resolved);
            }
            launch.resolution = std::move(resolved.resolution);
        } else {
            decision.refusal = refusalOf(resolved);
        }
        m_decision = std::move(decision);
    }
    if (m_resolving && m_source.ended()) {
        // The worker ended before it resolved the launch: the game cannot start, and nothing waits for it.
        const auto found = m_launches.find(*m_resolving);
        LaunchDecision decision;
        decision.token = *m_resolving;
        decision.info.gameId = found != m_launches.end() ? found->second.session.game.toString() : std::string();
        decision.info.title = found != m_launches.end() ? found->second.title : std::string();
        decision.refusal = LaunchMessage{std::string(lo::kNotReady), "unresolved", "the catalog worker ended"};
        m_decision = std::move(decision);
        m_resolving.reset();
    }
    // The directory's favourites and recent games (I4 step 7b): the answer to the latest request only.
    if (results.recentGames && m_recentAsked && results.recentGames->serial == *m_recentAsked) {
        m_recentAsked.reset();
        if (results.recentGames->error) {
            m_recentAnswer = Expected<std::vector<catalog::RecentGame>>(propagate(*results.recentGames->error));
        } else {
            m_recentAnswer = Expected<std::vector<catalog::RecentGame>>(std::move(results.recentGames->games));
        }
    }
    if (m_recentAsked && m_source.ended()) {
        m_recentAsked.reset();
        m_recentAnswer = Expected<std::vector<catalog::RecentGame>>(
            makeError(ErrorCode::Unavailable, "the catalog worker ended before it answered"));
    }
    collectSessions(results);
    collectEmulators(results);
    return CatalogChanges{collected.opened, collected.rowsChanged, refreshEnded, collected.refreshed && !otherImport,
                          otherImport};
}

void EsdeAppCatalog::collectEmulators(CatalogResults& results) {
    if (results.emulatorTable && m_tableAsked && results.emulatorTable->serial == m_emulatorSerial) {
        m_tableAsked = false;
    }
    if (results.emulatorTable) {
        m_table = std::move(results.emulatorTable);
        results.emulatorTable.reset();
    }
    if (results.statistics) {
        if (results.statistics->serial == m_statisticsSerial) {
            m_statisticsAsked = false;
            m_statistics = std::move(results.statistics);
        }
        results.statistics.reset();
    }
    if (results.gameControls) {
        // Only the latest question counts; an older status was still stored, and the latest answer reads it back.
        if (results.gameControls->serial == m_controlsSerial) {
            m_controlsAsked = 0;
            m_gameControls = std::move(results.gameControls);
        }
        results.gameControls.reset();
    }
    if (results.emulatorSamples) {
        m_samplesAsked = false;
        m_samples = std::move(results.emulatorSamples);
        results.emulatorSamples.reset();
    }
    for (ChoiceChanged& changed : results.changes) {
        m_changesAsked -= m_changesAsked > 0 ? 1 : 0;
        m_changes.push_back(std::move(changed));
    }
    results.changes.clear();
    for (LaunchTestResult& result : results.tests) {
        m_testAwaited = false;
        m_testResult = std::move(result);
    }
    results.tests.clear();
    if (m_source.ended()) {
        // Nothing asked of the worker will be answered any more.
        m_tableAsked = false;
        m_samplesAsked = false;
        m_changesAsked = 0;
        m_statisticsAsked = false;
        m_controlsAsked = 0;
        m_testAwaited = false;
    }
}

bool EsdeAppCatalog::collectRefresh(CatalogResults& results, bool ended) {
    if (results.refreshProgress) {
        m_refreshProgress = *results.refreshProgress;
        results.refreshProgress.reset();
    }
    if (!m_refreshRunning) {
        results.refreshed.reset();
        return false;
    }
    if (results.refreshed) {
        m_lastRefresh = std::move(results.refreshed->summary);
        results.refreshed.reset();
    } else if (ended) {
        // The worker ended before it answered (it failed, or stopped with the refresh still queued).
        RefreshSummary summary;
        summary.outcome = RefreshOutcome::Cancelled;
        summary.error = Error{ErrorCode::Cancelled, "the catalog worker ended", {}};
        m_lastRefresh = std::move(summary);
    } else {
        return false;
    }
    m_refreshRunning = false;
    m_refreshProgress.reset();
    m_lastRefreshLists = summarizeTimes(m_refreshLists);
    m_refreshLists.clear();
    return true;
}

bool EsdeAppCatalog::requestRefresh(RefreshRequest request) {
    if (m_stopped || m_refreshRunning || m_source.ended() || m_source.state() != CatalogState::Ready) {
        return false;
    }
    if (!m_worker.refresh(std::move(request))) {
        return false;
    }
    m_refreshRunning = true;
    m_refreshProgress.reset();
    m_refreshLists.clear();
    return true;
}

RefreshState EsdeAppCatalog::refreshState() const {
    RefreshState state;
    state.available = !m_stopped && !m_source.ended() && m_source.state() == CatalogState::Ready;
    state.running = m_refreshRunning;
    state.progress = m_refreshProgress;
    state.last = m_lastRefresh;
    state.lastLists = m_lastRefreshLists;
    state.otherImports = m_otherImports;
    return state;
}

void EsdeAppCatalog::collectSessions(CatalogResults& results) {
    for (StoredSession& stored : results.sessions) {
        const auto found = m_launches.find(stored.token);
        if (found != m_launches.end()) {
            found->second.stored = std::move(stored);
        }
    }
    results.sessions.clear();
}

void EsdeAppCatalog::sessionStarted(std::uint64_t token, const launcher::LaunchSession& session) {
    const auto found = m_launches.find(token);
    if (found == m_launches.end() || m_stopped) {
        return;
    }
    Launch& launch = found->second;
    launch.begun = session.times().begun;
    launch.session = startedPlaySession(launch.session, session, std::chrono::system_clock::now(), LaunchClock::now());
    m_worker.beginSession(token, launch.session);
}

void EsdeAppCatalog::sessionEnded(std::uint64_t token, const launcher::LaunchSession& session, LaunchEndCause cause,
                                  bool stoppedAwaiting) {
    const auto found = m_launches.find(token);
    if (found == m_launches.end() || m_stopped) {
        return;
    }
    Launch& launch = found->second;
    launch.session =
        finishedPlaySession(launch.session, session, cause, stoppedAwaiting, launch.guardAllowed, LaunchClock::now());
    m_worker.finishSession(token, launch.session, launch.system, session.request().executable);
    m_testAwaited = m_testAwaited || launch.test;
}

void EsdeAppCatalog::sessionRefused(std::uint64_t token) {
    const auto found = m_launches.find(token);
    if (found == m_launches.end() || m_stopped) {
        return;
    }
    Launch& launch = found->second;
    if (launch.system.str().empty()) {
        // A game that could not be resolved names no identity to record a session for.
        if (launch.test) {
            m_testResult = LaunchTestResult{token, "blocked", false, 0};
        }
        return;
    }
    launch.session = blockedPlaySession(launch.session, launch.resolution, std::chrono::system_clock::now());
    m_worker.finishSession(token, launch.session, launch.system, {});
    m_testAwaited = m_testAwaited || launch.test;
}

void EsdeAppCatalog::shutdown() {
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    const auto start = std::chrono::steady_clock::now();
    const bool joined = m_worker.stop();
    m_stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CatalogResults results = m_worker.take();
    collectSessions(results);
    // A refresh the stop cancelled answers with its summary; a worker left to end by itself answers nothing.
    static_cast<void>(collectRefresh(results, joined));
}

bool EsdeAppCatalog::requestRecentGames(std::size_t limit) {
    if (m_stopped) {
        return false;
    }
    m_recentAsked = ++m_recentSerial;
    m_recentAnswer.reset();
    m_worker.recentGames(*m_recentAsked, limit);
    return true;
}

std::string EsdeAppCatalog::systemFullName(const catalog::SystemName& system) const {
    const CatalogSystem* known = m_source.findSystem(system.str());
    return known != nullptr ? known->fullName : std::string();
}

void EsdeAppCatalog::pageShown() {
    if (!m_firstPageMs) {
        m_firstPageMs = sinceProcessStart(std::chrono::system_clock::now());
        spdlog::info("Catalog: first page shown {:.0f} ms after the process started", *m_firstPageMs);
    }
}

void EsdeAppCatalog::record(CatalogRecord& record) const {
    record.source = "esde";
    record.file = catalog_db::databaseFile(m_options.paths);
    record.notice = std::string(notice());
    record.library = m_source.state() == CatalogState::Ready ? m_source.library().str() : std::string();
    record.systems = m_source.systems().size();
    record.games = 0;
    for (const CatalogSystem& system : m_source.systems()) {
        record.games += system.games;
    }
    record.openedMs = m_openedMs;
    record.firstListMs = m_firstListMs;
    record.firstPageMs = m_firstPageMs;
    record.pages = m_source.pageTimes();
    std::vector<double> lists = m_listWaits;
    record.lists = summarizeTimes(lists);
    record.interruptedSessions = m_source.interruptedSessions();
}

void EsdeAppCatalog::recordLaunch(std::uint64_t token, LaunchRecord& record) const {
    const auto found = m_launches.find(token);
    if (found == m_launches.end() || !found->second.stored) {
        return;
    }
    const StoredSession& stored = *found->second.stored;
    if (stored.session.id != 0) {
        record.sessionId = stored.session.id;
    }
    record.sessionError = stored.error ? stored.error->toString() : std::string();
    record.verified = stored.verified;
}

} // namespace rv::app
