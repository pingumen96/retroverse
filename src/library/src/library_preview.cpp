#include <rv/library/library_preview.hpp>

#include <utility>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/esde/media_resolver.hpp>

namespace rv::library {
namespace {

std::string reasonOf(const catalog::CatalogEntry& entry) {
    std::string reason(catalog::toString(entry.notAGameReason));
    if (!entry.excludingSetting.empty()) {
        reason += ":" + entry.excludingSetting;
    }
    return reason;
}

} // namespace

void PreviewCounts::add(const PreviewCounts& other) {
    entries += other.entries;
    games += other.games;
    missing += other.missing;
    notAGame += other.notAGame;
    members += other.members;
    folders += other.folders;
    outside += other.outside;
    unavailable += other.unavailable;
    esde += other.esde;
    groups += other.groups;
    unavailableFolders += other.unavailableFolders;
    issues += other.issues;
    for (const auto& [reason, count] : other.reasons) {
        reasons[reason] += count;
    }
}

std::map<std::string, PreviewCounts> previewCounts(const catalog::LibrarySnapshot& snapshot) {
    std::map<std::string, PreviewCounts> counts;
    for (const catalog::SystemInfo& system : snapshot.systems) {
        counts[system.name.str()];
    }
    for (const catalog::CatalogEntry& entry : snapshot.entries) {
        PreviewCounts& count = counts[entry.system.str()];
        ++count.entries;
        count.unavailable += entry.availability == catalog::Availability::SourceUnavailable ? 1 : 0;
        const bool game = entry.state == catalog::EntryState::Game || entry.state == catalog::EntryState::DiscMember;
        count.esde += game && entry.availability == catalog::Availability::Available ? 1 : 0;
        switch (entry.state) {
        case catalog::EntryState::Game:
            ++count.games;
            break;
        case catalog::EntryState::MissingFile:
            ++count.missing;
            break;
        case catalog::EntryState::NotAGame:
            ++count.notAGame;
            ++count.reasons[reasonOf(entry)];
            break;
        case catalog::EntryState::DiscMember:
            ++count.members;
            break;
        case catalog::EntryState::Folder:
            ++count.folders;
            break;
        case catalog::EntryState::OutsideSystemFolder:
            ++count.outside;
            break;
        }
    }
    for (const catalog::DiscGroup& group : snapshot.discGroups) {
        ++counts[group.system.str()].groups;
    }
    for (const catalog::UnavailableFolder& folder : snapshot.unavailableFolders) {
        ++counts[folder.system.str()].unavailableFolders;
    }
    for (const catalog::Issue& issue : snapshot.issues) {
        if (const auto found = counts.find(issue.system); found != counts.end()) {
            ++found->second.issues;
        }
    }
    return counts;
}

Expected<LibraryPreview> previewLibrary(const catalog::LibraryProfile& profile, const PreviewOptions& options) {
    esde::EsdeLibrarySourceOptions sourceOptions;
    if (options.system) {
        sourceOptions.systems.push_back(*options.system);
    }
    esde::EsdeLibrarySource source(std::move(sourceOptions));
    catalog::LibraryLoadOptions loadOptions;
    loadOptions.cancel = options.cancel;
    auto snapshot = source.load(profile, loadOptions);
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    LibraryPreview preview;
    preview.snapshot = std::move(*snapshot);
    preview.statistics = source.lastStatistics();
    preview.counts = previewCounts(preview.snapshot);
    for (const catalog::SystemInfo& system : preview.snapshot.systems) {
        preview.total.add(preview.counts.at(system.name.str()));
    }
    return preview;
}

Expected<catalog::CoverageCounts> previewCoverage(const LibraryPreview& preview) {
    esde::EsdeMediaResolver resolver(mediaRootOf(preview.snapshot.profile));
    auto coverage = catalog::measureCoverage(preview.snapshot, resolver);
    if (!coverage) {
        return propagate(coverage.error());
    }
    return coverage->total[catalog::CoveragePopulation::Games];
}

esde::ResolvedRoot mediaRootOf(const catalog::LibraryProfile& profile) {
    const auto settings = esde::readSettings(profile.esdeHome);
    return esde::resolveMediaRoot(settings ? *settings : esde::EsdeSettings{}, profile);
}

} // namespace rv::library
