#include <rv/app/library_data.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <tuple>

#include <fmt/format.h>

#include <rv/catalog/import_service.hpp>
#include <rv/catalog/media.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

namespace rv::app {
namespace {

using catalog::CoveragePopulation;
using catalog::MediaKind;

/// The kinds the list shows, and that rank the systems.
constexpr std::array<MediaKind, 5> kShownKinds{MediaKind::Cover, MediaKind::Box3d, MediaKind::Screenshot,
                                               MediaKind::Marquee, MediaKind::Video};

[[nodiscard]] std::string shareOf(const catalog::CoverageCounts& counts, MediaKind kind) {
    return ui::shareText(counts.share(kind));
}

[[nodiscard]] ui::CoverageRow rowOf(const catalog::CoverageCounts& counts, std::string name, std::string fullName,
                                    std::uint64_t clipFailures) {
    ui::CoverageRow row;
    row.name = std::move(name);
    row.fullName = fullName.empty() ? row.name : std::move(fullName);
    row.games = counts.known();
    row.covers = shareOf(counts, MediaKind::Cover);
    row.boxes = shareOf(counts, MediaKind::Box3d);
    row.screenshots = shareOf(counts, MediaKind::Screenshot);
    row.marquees = shareOf(counts, MediaKind::Marquee);
    row.videos = shareOf(counts, MediaKind::Video);
    row.any = ui::shareText(counts.anyShare());
    for (const MediaKind kind : catalog::kMediaKinds) {
        const std::uint64_t with = counts.with(kind);
        row.kinds.push_back(ui::CoverageShare{std::string(catalog::toString(kind)), shareOf(counts, kind), with,
                                              counts.known() - std::min(with, counts.known()),
                                              counts.known() > 0 && with * 2 < counts.known()});
    }
    row.clipFailures = clipFailures;
    return row;
}

/// The sum of the shown kinds' members, the share's numerator of the mean (its denominator: five times the known).
[[nodiscard]] std::uint64_t shownWith(const catalog::CoverageCounts& counts) {
    std::uint64_t sum = 0;
    for (const MediaKind kind : kShownKinds) {
        sum += counts.with(kind);
    }
    return sum;
}

[[nodiscard]] ui::CoverageTable tableOf(const CoverageList& list, CoveragePopulation population,
                                        const std::map<std::string, std::uint64_t, std::less<>>& clips) {
    const auto fullNameOf = [&](const std::string& name) {
        const auto found = std::lower_bound(list.fullNames.begin(), list.fullNames.end(), name,
                                            [](const auto& entry, const std::string& key) { return entry.first < key; });
        return found != list.fullNames.end() && found->first == name ? found->second : std::string();
    };
    struct Ranked {
        const catalog::SystemCoverage* system;
        std::string fullName;
    };
    std::vector<Ranked> ranked;
    for (const catalog::SystemCoverage& system : list.coverage.systems) {
        if (system[population].known() > 0) {
            std::string fullName = fullNameOf(system.system.str());
            ranked.push_back(Ranked{&system, fullName.empty() ? system.system.str() : std::move(fullName)});
        }
    }
    // Worst first: the mean of the shown kinds' shares, compared as fractions (integers: no rounding decides).
    std::sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) {
        const catalog::CoverageCounts& left = (*a.system)[population];
        const catalog::CoverageCounts& right = (*b.system)[population];
        const std::uint64_t l = shownWith(left) * right.known();
        const std::uint64_t r = shownWith(right) * left.known();
        if (l != r) {
            return l < r;
        }
        return std::tie(a.fullName, a.system->system) < std::tie(b.fullName, b.system->system);
    });
    ui::CoverageTable table;
    std::uint64_t allClips = 0;
    for (const auto& [system, count] : clips) {
        allClips += count;
    }
    table.total = rowOf(list.coverage.total[population], {}, {}, allClips);
    for (const Ranked& entry : ranked) {
        const std::string& name = entry.system->system.str();
        const auto found = clips.find(name);
        table.systems.push_back(rowOf((*entry.system)[population], name, entry.fullName,
                                      found != clips.end() ? found->second : 0));
    }
    return table;
}

[[nodiscard]] int severityRank(std::string_view severity) noexcept {
    return severity == "error" ? 2 : severity == "warning" ? 1 : 0;
}

} // namespace

ui::CoverageContent coverageContentOf(const CoverageList& list) {
    ui::CoverageContent content;
    std::map<std::string, std::uint64_t, std::less<>> clips;
    for (const ClipFailure& clip : list.clips) {
        ++clips[clip.system];
        content.clips.push_back(ui::CoverageClip{clip.system, clip.file, clip.verdict});
    }
    content.clipsUnknown = list.clipsError.has_value();
    content.games = tableOf(list, CoveragePopulation::Games, clips);
    content.scraped = tableOf(list, CoveragePopulation::ScrapedGames, clips);
    content.empty = list.coverage.total[CoveragePopulation::Games].entries == 0;
    return content;
}

std::string_view issueGroupOf(std::string_view code) noexcept {
    if (code == catalog::kImportIssueMassDisappearance) {
        return "guard";
    }
    if (code == catalog::kImportIssueRomRootUnavailable) {
        return "rom-root";
    }
    if (code == "system-folder-unavailable" || code == "system-folder-missing") {
        return "system-folder";
    }
    if (code == catalog::kImportIssueSystemNotConfigured || code == "system-not-loaded" ||
        code == "system-fallback-used" || code == "gamelist-location-unsupported") {
        return "system-config";
    }
    if (code == catalog::kImportIssueGamelistKept || code == "gamelist-failed") {
        return "gamelist";
    }
    if (code.starts_with("gamelist-")) {
        return "gamelist-entry";
    }
    if (code.starts_with("rom-")) {
        return "rom-entry";
    }
    if (code == catalog::kImportIssueKeyCollision) {
        return "key-collision";
    }
    if (code.starts_with("disc-")) {
        return "disc";
    }
    if (code == "media-root-unavailable") {
        return "media-root";
    }
    if (code.starts_with("media-")) {
        return "media";
    }
    if (code.starts_with("mame-")) {
        return "mame";
    }
    return "other";
}

ui::LibraryContent libraryContentOf(const LibraryStatus& status, const LocalDate& localDate) {
    ui::LibraryContent content;
    for (const LibraryFolder& folder : status.folders) {
        ui::LibraryRoot root;
        root.kind = folder.kind;
        root.system = folder.system;
        root.path = folder.folder.empty() ? std::string() : core::pathToUtf8Lossy(folder.folder);
        root.state = !folder.reachable ? "unknown" : *folder.reachable ? "reachable" : "unreachable";
        content.roots.push_back(std::move(root));
    }
    content.unavailableSystems = status.unavailableSystems;
    if (status.lastImport) {
        const catalog::ImportRecord& record = *status.lastImport;
        content.lastImport.known = true;
        content.lastImport.kind = std::string(catalog::toString(record.kind));
        content.lastImport.systems = record.systemsAsked.size();
        content.lastImport.acceptRemovals = record.acceptRemovals;
        if (auto finished = core::parseUtcIso8601(record.finishedAt); finished && localDate) {
            if (const std::optional<ui::DirectoryDate> day = localDate(*finished)) {
                content.lastImport.when = fmt::format("{:02}/{:02}/{:04}", day->day, day->month, day->year);
            }
        }
    }
    // The issues by kind, in the order first met; each kind's worst severity and systems.
    struct Group {
        ui::LibraryIssueGroup shown;
        std::vector<std::string> systems;
        std::size_t first = 0;
    };
    std::vector<Group> groups;
    for (const catalog::Issue& issue : status.issues) {
        const std::string_view kind = issueGroupOf(issue.code);
        auto found = std::find_if(groups.begin(), groups.end(),
                                  [&](const Group& group) { return group.shown.code == kind; });
        if (found == groups.end()) {
            groups.push_back(Group{ui::LibraryIssueGroup{std::string(kind), "info", 0, {}, 0}, {}, groups.size()});
            found = groups.end() - 1;
        }
        ++found->shown.count;
        const std::string_view severity = catalog::toString(issue.severity);
        if (severityRank(severity) > severityRank(found->shown.severity)) {
            found->shown.severity = std::string(severity);
        }
        if (!issue.system.empty() &&
            std::find(found->systems.begin(), found->systems.end(), issue.system) == found->systems.end()) {
            found->systems.push_back(issue.system);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        const int left = severityRank(a.shown.severity);
        const int right = severityRank(b.shown.severity);
        if (left != right) {
            return left > right;
        }
        if (a.shown.count != b.shown.count) {
            return a.shown.count > b.shown.count;
        }
        return a.first < b.first;
    });
    for (Group& group : groups) {
        std::string systems;
        for (std::size_t index = 0; index < group.systems.size() && index < ui::library::kIssueSystems; ++index) {
            systems += (index == 0 ? "" : ", ") + group.systems[index];
        }
        group.shown.systems = std::move(systems);
        group.shown.moreSystems = group.systems.size() > ui::library::kIssueSystems
                                      ? group.systems.size() - ui::library::kIssueSystems
                                      : 0;
        content.issues.push_back(std::move(group.shown));
    }
    content.issueCount = status.issues.size();
    for (const HeldSystem& held : status.held) {
        content.held.push_back(ui::LibraryHeld{held.system.str(), held.fullName, held.gone, held.present});
    }
    for (const ReviewState& review : status.reviews) {
        ui::LibraryReview shown;
        shown.id = review.id;
        shown.gameId = review.game.toString();
        shown.system = review.fullName.empty() ? review.system.str() : review.fullName;
        shown.title = review.title;
        shown.vanished = review.vanished;
        for (const ReviewCandidateState& candidate : review.candidates) {
            shown.candidates.push_back(
                ui::LibraryCandidate{candidate.path, candidate.game ? candidate.game->toString() : std::string()});
        }
        content.reviews.push_back(std::move(shown));
    }
    content.files = status.hashes.files;
    content.hashed = status.hashes.current;
    return content;
}

ui::LibraryLive libraryLiveOf(const RefreshState& refresh, const QuickHashState& hash, const WatchStatus& watch) {
    ui::LibraryLive live;
    live.canRefresh = refresh.available;
    live.refreshing = refresh.running;
    if (refresh.running && refresh.progress) {
        live.phase = std::string(toString(refresh.progress->phase));
        live.refreshDone = refresh.progress->done;
        live.refreshTotal = refresh.progress->total;
    } else if (refresh.running) {
        live.phase = std::string(toString(RefreshPhase::Reading));
    }
    if (refresh.last) {
        live.lastRefresh = std::string(toString(refresh.last->outcome));
        live.added = refresh.last->added;
        live.removed = refresh.last->removed;
        live.guarded = refresh.last->guarded;
    }
    if (hash.running) {
        live.hash = hash.cancelling ? "cancelling" : "running";
        if (hash.progress) {
            live.hashDone = hash.progress->done;
            live.hashTotal = hash.progress->total;
            live.hashHashed = hash.progress->hashed;
        }
    } else if (hash.last) {
        live.hash = hash.last->outcome;
        live.hashHashed = hash.last->result.hashed;
        live.hashUnreadable = hash.last->result.unreadable;
        live.hashRemaining = hash.last->result.remaining;
        live.hashDone = hash.last->result.candidates - hash.last->result.remaining;
        live.hashTotal = hash.last->result.candidates;
    }
    live.watching = watch.watching;
    live.modes = watch.modes;
    return live;
}

} // namespace rv::app
