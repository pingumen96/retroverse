// retroverse-cli media --check-videos (I5 step 7, ADR-0016 §8): the library-wide check of the clips. Each clip of the
// media root's videos folders (ES-DE's: <media root>/<system>/videos, as EsdeMediaResolver finds the folders; files of
// ES-DE's video extensions) without a verdict for its file as it is now is checked (media::checkVideo: its head, middle
// and end decoded by the screens' own decoder) on `--jobs` threads at below-normal priority, and its verdict merged
// into <user data>/cache/media/videos.json every few clips (media::updateVideoVerdicts), so that a run interrupted at
// any point resumes where it stopped; a merge that fails (the app holds the lock, the file cannot be written) keeps its
// verdicts for the next one. `--recheck-failures` checks the clips with a failure verdict again. A clip the system
// could not read (DecodeFailure::ReadError) or that went meanwhile gets no verdict: it is listed as not checked.
// Verdicts of clips no longer in a folder listed whole are removed. Nothing is written under the media root: the clips
// are only read. The report counts the verdicts per kind and system (those of earlier runs and of the app included),
// lists the failures (with the decoder's reason for those checked now) and the near misses (playable clips in which the
// decoder counted errors or damaged frames: ADR-0004's thresholds), and the clips whose check did not finish.

#include "esde_text.hpp"
#include "media_videos.hpp"

#include <rv/catalog/media.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/media/video_check.hpp>
#include <rv/media/video_verdicts.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rv::cli {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using media::DecodeFailure;

constexpr std::string_view kName = "retroverse-cli media --check-videos";
/// Verdicts are merged into the file after this many clips, or this long after the last merge.
constexpr std::size_t kMergeEvery = 25;
constexpr std::chrono::seconds kMergeInterval{20};
/// A progress line on stderr this often.
constexpr std::chrono::seconds kProgressInterval{30};

/// The verdicts a report counts, in its columns' order: playable, then each failure.
constexpr std::array<DecodeFailure, 7> kVerdictColumns{DecodeFailure::None,          DecodeFailure::InvalidFile,
                                                       DecodeFailure::NoVideoStream, DecodeFailure::UnsupportedCodec,
                                                       DecodeFailure::CorruptData,   DecodeFailure::Truncated,
                                                       DecodeFailure::DecoderError};
constexpr std::array<std::string_view, 7> kVerdictTitles{"playable", "invalid",   "no-video", "codec",
                                                         "corrupt",  "truncated", "decoder"};

std::size_t columnOf(DecodeFailure failure) {
    const auto found = std::find(kVerdictColumns.begin(), kVerdictColumns.end(), failure);
    return static_cast<std::size_t>(found - kVerdictColumns.begin());
}

/// One clip of a videos folder.
struct Clip {
    std::size_t system = 0;
    /// Below the media root, '/' between the elements, as the folders spell it.
    std::string path;
    media::VideoFileFacts facts;
};

/// One system's videos folder.
struct SystemScan {
    catalog::SystemName name;
    /// "listed", "missing" (no media or videos folder: no clips), "unavailable" (it could not be read).
    std::string folder = "missing";
    /// Its clips' prefix below the media root ("snes/videos/"), when listed.
    std::string prefix;
    /// Listed whole: verdicts below it of clips not listed are removed.
    bool whole = false;
    std::string problem;
};

/// What the report counts of a system (or of all).
struct Counts {
    std::uint64_t clips = 0;
    std::uint64_t checked = 0;
    std::uint64_t known = 0;
    std::uint64_t unchecked = 0;
    std::array<std::uint64_t, kVerdictColumns.size()> verdicts{};

    void add(const Counts& other) {
        clips += other.clips;
        checked += other.checked;
        known += other.known;
        unchecked += other.unchecked;
        for (std::size_t index = 0; index < verdicts.size(); ++index) {
            verdicts[index] += other.verdicts[index];
        }
    }
};

/// A failure or a near miss the report lists.
struct Listed {
    std::string system;
    std::string path;
    const media::VideoVerdict* verdict = nullptr;
    std::string detail;
};

bool isVideoName(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const auto extension = core::fileSystemUpperCase(name.substr(dot));
    if (!extension) {
        return false;
    }
    return std::any_of(esde::kVideoExtensions.begin(), esde::kVideoExtensions.end(), [&](std::string_view candidate) {
        return core::fileSystemUpperCase(candidate).value_or(std::string()) == *extension;
    });
}

/// The process's priority class, below normal while it lives (ADR-0016 §8: the check runs at below-normal priority,
/// its decoders' threads included).
class BelowNormalPriority {
public:
    BelowNormalPriority() : m_previous(GetPriorityClass(GetCurrentProcess())) {
        m_set = SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) != 0;
    }
    BelowNormalPriority(const BelowNormalPriority&) = delete;
    BelowNormalPriority& operator=(const BelowNormalPriority&) = delete;
    ~BelowNormalPriority() {
        if (m_set && m_previous != 0) {
            SetPriorityClass(GetCurrentProcess(), m_previous);
        }
    }

private:
    DWORD m_previous = 0;
    bool m_set = false;
};

/// The default logger at warn while it lives (unless it logs more already): a scan opens thousands of decoders, whose
/// every open and end would be an info line.
class QuietDecoders {
public:
    QuietDecoders() {
        if (const auto logger = spdlog::default_logger(); logger && logger->level() == spdlog::level::info) {
            m_logger = logger;
            logger->set_level(spdlog::level::warn);
        }
    }
    QuietDecoders(const QuietDecoders&) = delete;
    QuietDecoders& operator=(const QuietDecoders&) = delete;
    ~QuietDecoders() {
        if (m_logger) {
            m_logger->set_level(spdlog::level::info);
        }
    }

private:
    std::shared_ptr<spdlog::logger> m_logger;
};

/// Lists the videos folder of each system into `clips`.
void listClips(esde::EsdeMediaResolver& resolver, const fs::path& root, std::vector<SystemScan>& scans,
               std::vector<Clip>& clips) {
    for (std::size_t index = 0; index < scans.size(); ++index) {
        SystemScan& scan = scans[index];
        const auto listing = resolver.listing(scan.name);
        if (!listing) {
            scan.folder = "unavailable";
            scan.problem = listing.error().toString();
            continue;
        }
        const esde::SystemMediaListing& system = **listing;
        const esde::MediaKindListing& videos = system[catalog::MediaKind::Video];
        if (system.state == esde::MediaFolderState::Unavailable ||
            videos.state == esde::MediaFolderState::Unavailable) {
            scan.folder = "unavailable";
            scan.problem = system.state == esde::MediaFolderState::Unavailable ? system.detail : videos.detail;
            continue;
        }
        if (system.state == esde::MediaFolderState::Missing || videos.state == esde::MediaFolderState::Missing) {
            scan.folder = "missing";
            continue;
        }
        scan.folder = "listed";
        scan.prefix = system.folderName + "/" + videos.folderName + "/";
        const auto folder = core::extendedLengthPathBelow(root, system.folderName + "/" + videos.folderName);
        if (!folder) {
            scan.folder = "unavailable";
            scan.problem = folder.error().toString();
            continue;
        }
        bool whole = true;
        const auto walked = core::walkTree(*folder, [&](const core::TreeEntry& entry) {
            if (entry.kind == core::TreeEntryKind::Error) {
                whole = false;
                if (scan.problem.empty()) {
                    scan.problem = entry.error.toString();
                }
            } else if (entry.kind == core::TreeEntryKind::File && isVideoName(entry.relativePath)) {
                clips.push_back(
                    Clip{index, scan.prefix + entry.relativePath,
                         media::VideoFileFacts{entry.size, entry.lastWriteTime.time_since_epoch().count()}});
            }
            return core::WalkAction::Continue;
        });
        if (!walked) {
            scan.folder = "unavailable";
            scan.problem = walked.error().toString();
            whole = false;
        }
        scan.whole = whole;
    }
}

/// The checks of `tasks` (indices into `clips`) on `jobs` threads; `onResult` runs on the calling thread for each.
class CheckRunner {
public:
    CheckRunner(const fs::path& root, const std::vector<Clip>& clips, std::vector<std::size_t> tasks, int jobs)
        : m_root(root), m_clips(clips), m_tasks(std::move(tasks)) {
        const int threads = std::max(1, std::min<int>(jobs, static_cast<int>(m_tasks.size())));
        for (int thread = 0; thread < threads && !m_tasks.empty(); ++thread) {
            m_threads.emplace_back([this] { work(); });
        }
    }
    CheckRunner(const CheckRunner&) = delete;
    CheckRunner& operator=(const CheckRunner&) = delete;
    ~CheckRunner() {
        m_cancel = true;
        m_threads.clear(); // joined: each finishes its clip (bounded by the check's time limit)
    }

    /// The next result (its task index into `clips` and what the check found), waiting at most `wait`; none when no
    /// result came, or every task is done.
    std::optional<std::pair<std::size_t, media::VideoCheckResult>> next(std::chrono::milliseconds wait) {
        std::unique_lock lock(m_mutex);
        m_ready.wait_for(lock, wait, [this] { return !m_done.empty(); });
        if (m_done.empty()) {
            return std::nullopt;
        }
        auto result = std::move(m_done.front());
        m_done.pop_front();
        return result;
    }

private:
    void work() {
        while (!m_cancel) {
            const std::size_t task = m_next.fetch_add(1);
            if (task >= m_tasks.size()) {
                return;
            }
            const Clip& clip = m_clips[m_tasks[task]];
            media::VideoCheckResult result;
            if (const auto path = core::extendedLengthPathBelow(m_root, clip.path)) {
                media::VideoCheckSettings settings;
                settings.cancel = &m_cancel;
                result = media::checkVideo(*path, settings);
            } else {
                result.detail = path.error().toString();
            }
            const std::lock_guard lock(m_mutex);
            m_done.emplace_back(m_tasks[task], std::move(result));
            m_ready.notify_one();
        }
    }

    const fs::path& m_root;
    const std::vector<Clip>& m_clips;
    std::vector<std::size_t> m_tasks;
    std::atomic<std::size_t> m_next{0};
    std::atomic<bool> m_cancel{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<std::pair<std::size_t, media::VideoCheckResult>> m_done;
    std::vector<std::jthread> m_threads;
};

std::string verdictsText(const Counts& counts) {
    std::string text;
    for (const std::uint64_t count : counts.verdicts) {
        text += fmt::format("{:>10}", count);
    }
    return text;
}

Json countsJson(const Counts& counts) {
    Json verdicts = Json::object();
    for (std::size_t index = 0; index < kVerdictColumns.size(); ++index) {
        verdicts[media::verdictName(kVerdictColumns[index])] = counts.verdicts[index];
    }
    return Json{{"clips", counts.clips},
                {"checked", counts.checked},
                {"known", counts.known},
                {"unchecked", counts.unchecked},
                {"verdicts", std::move(verdicts)}};
}

} // namespace

ExitCode checkVideos(CommandContext& context, Output& out, const esde::ResolvedRoot& mediaRoot,
                     const esde::SystemsConfiguration& systems, const std::optional<catalog::SystemName>& system,
                     int jobs, bool recheckFailures, bool json) {
    const Clock::time_point started = Clock::now();
    esde::EsdeMediaResolver resolver(mediaRoot);
    if (const Status root = resolver.checkRoot(); !root || !resolver.mediaRoot()) {
        context.err.writeLine(fmt::format("{}: no clip can be checked: {}", kName,
                                          root ? std::string("the media root is not known") : root.error().toString()));
        return ExitCode::ProblemsFound;
    }
    const fs::path root = *resolver.mediaRoot();
    std::vector<SystemScan> scans;
    for (const esde::SystemDefinition& definition : systems.systems) {
        if (!system || definition.name == *system) {
            scans.push_back(SystemScan{definition.name});
        }
    }
    std::vector<Clip> clips;
    listClips(resolver, root, scans, clips);

    const fs::path file = media::videoVerdictsFile(context.paths.cacheDir());
    auto stored = media::readVideoVerdicts(file);
    if (!stored) {
        if (!media::isUnreadableText(stored.error().code)) {
            // Its verdicts may all be there: checking every clip again for nothing, and failing to merge, helps no one.
            context.err.writeLine(fmt::format("{}: the verdicts cannot be read: {}", kName, stored.error().toString()));
            return ExitCode::RuntimeFailure;
        }
        spdlog::warn("Video verdicts: {} holds no verdicts ({}); it is written anew", pathText(file),
                     stored.error().toString());
        stored = media::VideoVerdicts{};
    }
    media::VideoVerdicts verdicts = std::move(*stored);

    // The clips whose file has no verdict as it is now (and those with a failure's, with --recheck-failures).
    std::vector<std::size_t> tasks;
    std::size_t rechecked = 0;
    for (std::size_t index = 0; index < clips.size(); ++index) {
        const media::VideoVerdict* verdict = verdicts.findCurrent(clips[index].path, clips[index].facts);
        if (verdict == nullptr) {
            tasks.push_back(index);
        } else if (recheckFailures && !verdict->playable()) {
            tasks.push_back(index);
            ++rechecked;
        }
    }
    const std::size_t known = clips.size() - tasks.size();
    spdlog::info(
        "Video check: {} clip(s) in {} system(s), {} with a verdict for their file as it is, {} to check on {} "
        "job(s) ({} failure(s) checked again)",
        clips.size(), scans.size(), known, tasks.size(), jobs, rechecked);

    std::vector<std::string> details(clips.size());
    std::vector<bool> checkedNow(clips.size(), false);
    std::vector<std::size_t> incomplete;
    // The verdicts not merged into the file yet: a merge that fails keeps them for the next one.
    std::vector<media::VideoVerdict> pending;
    const auto merge = [&](std::span<const std::string> removals) {
        if (pending.empty() && removals.empty()) {
            return true;
        }
        auto merged = media::updateVideoVerdicts(file, pending, removals);
        if (!merged) {
            context.err.writeLine(fmt::format("{}: {} verdict(s) not written yet (kept for the next merge): {}", kName,
                                              pending.size(), merged.error().toString()));
            return false;
        }
        verdicts = std::move(*merged);
        pending.clear();
        return true;
    };
    {
        const BelowNormalPriority priority;
        const QuietDecoders quiet;
        CheckRunner runner(root, clips, tasks, jobs);
        Clock::time_point lastMerge = Clock::now();
        Clock::time_point lastProgress = Clock::now();
        std::size_t done = 0;
        std::size_t failures = 0;
        while (done < tasks.size()) {
            if (auto result = runner.next(std::chrono::milliseconds(500))) {
                ++done;
                const auto& [index, check] = *result;
                const Clip& clip = clips[index];
                details[index] = check.detail;
                if (!check.completed || check.failure == DecodeFailure::FileNotFound ||
                    check.failure == DecodeFailure::ReadError) {
                    // No verdict: timed out, the file went meanwhile, or the system could not read it.
                    incomplete.push_back(index);
                } else {
                    media::VideoVerdict verdict;
                    verdict.path = clip.path;
                    verdict.file = clip.facts;
                    verdict.failure = check.failure;
                    if (check.info) {
                        media::describeStream(verdict, *check.info);
                    }
                    verdict.decodeErrors = check.stats.decodeErrors;
                    verdict.damagedFrames = check.stats.damagedFrames;
                    verdict.source = media::VerdictSource::Check;
                    failures += verdict.playable() ? 0 : 1;
                    checkedNow[index] = true;
                    pending.push_back(std::move(verdict));
                }
            }
            if (pending.size() >= kMergeEvery || (!pending.empty() && Clock::now() - lastMerge >= kMergeInterval)) {
                static_cast<void>(merge({})); // a failure keeps them: the next merge tries again
                lastMerge = Clock::now();
            }
            if (Clock::now() - lastProgress >= kProgressInterval) {
                lastProgress = Clock::now();
                context.err.writeLine(fmt::format("{}: {} of {} clip(s) checked, {} failure(s), {:.0f} s", kName, done,
                                                  tasks.size(), failures,
                                                  std::chrono::duration<double>(Clock::now() - started).count()));
            }
        }
    }
    // The verdicts of clips no longer in a folder listed whole go.
    std::set<std::string, std::less<>> listedKeys;
    for (const Clip& clip : clips) {
        if (auto key = media::verdictKey(clip.path)) {
            listedKeys.insert(std::move(*key));
        }
    }
    std::vector<std::string> removals;
    for (const SystemScan& scan : scans) {
        if (!scan.whole) {
            continue;
        }
        const std::string prefix = core::fileSystemUpperCase(scan.prefix).value_or(scan.prefix);
        for (const media::VideoVerdict* verdict : verdicts.entries()) {
            const std::optional<std::string> key = media::verdictKey(verdict->path);
            if (key && key->starts_with(prefix) && !listedKeys.contains(*key)) {
                removals.push_back(verdict->path);
            }
        }
    }
    // The last merge, with the removals; if it fails too, the report still shows the verdicts found.
    const bool writeFailed = !merge(removals);
    for (media::VideoVerdict& verdict : pending) {
        static_cast<void>(verdicts.set(std::move(verdict)));
    }
    pending.clear();

    // The report: every listed clip's verdict as the file holds it now.
    std::vector<Counts> perSystem(scans.size());
    std::vector<Listed> failed;
    std::vector<Listed> nearMisses;
    for (std::size_t index = 0; index < clips.size(); ++index) {
        const Clip& clip = clips[index];
        Counts& counts = perSystem[clip.system];
        ++counts.clips;
        const media::VideoVerdict* verdict = verdicts.findCurrent(clip.path, clip.facts);
        if (verdict == nullptr) {
            ++counts.unchecked;
            continue;
        }
        ++(checkedNow[index] ? counts.checked : counts.known);
        ++counts.verdicts[columnOf(verdict->failure)];
        const std::string systemName = scans[clip.system].name.str();
        if (!verdict->playable()) {
            failed.push_back(Listed{systemName, clip.path, verdict, details[index]});
        } else if (verdict->decodeErrors + verdict->damagedFrames > 0) {
            nearMisses.push_back(Listed{systemName, clip.path, verdict, {}});
        }
    }
    Counts total;
    for (const Counts& counts : perSystem) {
        total.add(counts);
    }
    const bool unreadable = std::any_of(scans.begin(), scans.end(), [](const SystemScan& scan) {
        return scan.folder == "unavailable" || (scan.folder == "listed" && !scan.whole);
    });
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const ExitCode code = writeFailed                         ? ExitCode::RuntimeFailure
                          : unreadable || !incomplete.empty() ? ExitCode::ProblemsFound
                                                              : ExitCode::Ok;
    spdlog::info("Video check: {} clip(s) checked in {:.1f} s, {} known, {} without a verdict, {} removed",
                 total.checked, seconds, total.known, total.unchecked, removals.size());

    if (json) {
        Json systemsJson = Json::array();
        for (std::size_t index = 0; index < scans.size(); ++index) {
            Json entry{{"system", scans[index].name.str()}, {"videosFolder", scans[index].folder}};
            if (!scans[index].problem.empty()) {
                entry["problem"] = scans[index].problem;
            }
            entry.update(countsJson(perSystem[index]));
            systemsJson.push_back(std::move(entry));
        }
        Json failures = Json::array();
        for (const Listed& listed : failed) {
            failures.push_back(Json{{"system", listed.system},
                                    {"path", listed.path},
                                    {"verdict", media::verdictName(listed.verdict->failure)},
                                    {"source", std::string(media::toString(listed.verdict->source))},
                                    {"durationMs", listed.verdict->durationMs},
                                    {"codec", listed.verdict->codec},
                                    {"detail", listed.detail}});
        }
        Json closeCalls = Json::array();
        for (const Listed& listed : nearMisses) {
            closeCalls.push_back(Json{{"system", listed.system},
                                      {"path", listed.path},
                                      {"decodeErrors", listed.verdict->decodeErrors},
                                      {"damagedFrames", listed.verdict->damagedFrames}});
        }
        Json unfinished = Json::array();
        for (const std::size_t index : incomplete) {
            unfinished.push_back(Json{{"path", clips[index].path}, {"detail", details[index]}});
        }
        Json report{{"mediaRoot", rootJson(mediaRoot)},
                    {"verdictsFile", pathText(file)},
                    {"jobs", jobs},
                    {"seconds", seconds},
                    {"total", countsJson(total)},
                    {"systems", std::move(systemsJson)},
                    {"failures", std::move(failures)},
                    {"nearMisses", std::move(closeCalls)},
                    {"incomplete", std::move(unfinished)},
                    {"removed", removals.size()}};
        out.write(report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return code;
    }

    out.writeLine(fmt::format("{:<14}{}", "Media root", rootText(mediaRoot, "MediaDirectory", "unknown")));
    out.writeLine(fmt::format("{:<14}{}", "Verdicts", pathText(file)));
    out.writeLine(
        fmt::format("{:<14}{} clip(s): {} checked now on {} job(s) in {:.1f} s, {} known, {} without a verdict",
                    "Clips", total.clips, total.checked, jobs, seconds, total.known, total.unchecked));
    out.writeLine();
    std::string header = fmt::format("{:<14}{:>8}", "System", "Clips");
    for (const std::string_view title : kVerdictTitles) {
        header += fmt::format("{:>10}", title);
    }
    out.writeLine(header);
    for (std::size_t index = 0; index < scans.size(); ++index) {
        if (perSystem[index].clips == 0 && scans[index].folder != "unavailable") {
            continue;
        }
        std::string line = fmt::format("{:<14}{:>8}{}", scans[index].name.str(), perSystem[index].clips,
                                       verdictsText(perSystem[index]));
        if (scans[index].folder == "unavailable" || !scans[index].problem.empty()) {
            line += fmt::format("  (videos folder {}: {})", scans[index].folder, scans[index].problem);
        }
        out.writeLine(line);
    }
    out.writeLine(fmt::format("{:<14}{:>8}{}", "All", total.clips, verdictsText(total)));
    if (!failed.empty()) {
        out.writeLine();
        out.writeLine(fmt::format("Failures ({})", failed.size()));
        for (const Listed& listed : failed) {
            out.writeLine(fmt::format("  {:<18}{}{}", media::verdictName(listed.verdict->failure), listed.path,
                                      listed.detail.empty() ? std::string() : ": " + listed.detail));
        }
    }
    if (!nearMisses.empty()) {
        out.writeLine();
        out.writeLine(fmt::format("Playable with errors ({}): decode errors and damaged frames below the thresholds",
                                  nearMisses.size()));
        for (const Listed& listed : nearMisses) {
            out.writeLine(fmt::format("  {:>4} {:>4}  {}", listed.verdict->decodeErrors, listed.verdict->damagedFrames,
                                      listed.path));
        }
    }
    if (!incomplete.empty()) {
        out.writeLine();
        out.writeLine(fmt::format("Not checked ({})", incomplete.size()));
        for (const std::size_t index : incomplete) {
            out.writeLine(fmt::format("  {}: {}", clips[index].path, details[index]));
        }
    }
    if (!removals.empty()) {
        out.writeLine();
        out.writeLine(fmt::format("{} verdict(s) of clips no longer there removed", removals.size()));
    }
    return code;
}

} // namespace rv::cli
