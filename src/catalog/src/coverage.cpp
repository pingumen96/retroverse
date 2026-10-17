#include <rv/catalog/coverage.hpp>

#include <algorithm>

#include <fmt/format.h>

namespace rv::catalog {

bool inPopulation(const CatalogEntry& entry, CoveragePopulation population) noexcept {
    switch (population) {
    case CoveragePopulation::GamelistEntries:
        return entry.fromGamelist() && entry.state != EntryState::Folder;
    case CoveragePopulation::GamelistEntriesWithFile:
        return entry.fromGamelist() && entry.state != EntryState::Folder &&
               entry.availability == Availability::Available && entry.file.has_value();
    case CoveragePopulation::ScrapedGames:
        return entry.fromGamelist() && entry.state == EntryState::Game;
    case CoveragePopulation::Games:
        return entry.state == EntryState::Game;
    }
    return false;
}

std::optional<double> CoverageCounts::share(MediaKind kind) const noexcept {
    if (known() == 0) {
        return std::nullopt;
    }
    return static_cast<double>(with(kind)) / static_cast<double>(known());
}

std::optional<double> CoverageCounts::anyShare() const noexcept {
    if (known() == 0) {
        return std::nullopt;
    }
    return static_cast<double>(withAny) / static_cast<double>(known());
}

void CoverageCounts::add(const CoverageCounts& other) noexcept {
    entries += other.entries;
    unknown += other.unknown;
    withAny += other.withAny;
    for (std::size_t i = 0; i < kMediaKindCount; ++i) {
        withKind[i] += other.withKind[i];
    }
}

const SystemCoverage* MediaCoverage::find(const SystemName& name) const noexcept {
    const auto found = std::lower_bound(systems.begin(), systems.end(), name,
                                        [](const SystemCoverage& a, const SystemName& b) { return a.system < b; });
    return found != systems.end() && found->system == name ? &*found : nullptr;
}

SystemCoverage& CoverageBuilder::systemOf(const SystemName& system) {
    auto found = std::lower_bound(m_systems.begin(), m_systems.end(), system,
                                  [](const SystemCoverage& a, const SystemName& b) { return a.system < b; });
    if (found == m_systems.end() || found->system != system) {
        found = m_systems.insert(found, SystemCoverage{system, {}});
    }
    return *found;
}

CoverageBuilder::Unknown& CoverageBuilder::unknownOf(const SystemName& system) {
    auto found = std::lower_bound(m_unknown.begin(), m_unknown.end(), system,
                                  [](const Unknown& a, const SystemName& b) { return a.system < b; });
    if (found == m_unknown.end() || found->system != system) {
        found = m_unknown.insert(found, Unknown{system, 0, std::string()});
    }
    return *found;
}

void CoverageBuilder::add(const CatalogEntry& entry, const EntryMedia* media) {
    bool member = false;
    for (const CoveragePopulation population : kCoveragePopulations) {
        member = member || inPopulation(entry, population);
    }
    if (!member) {
        return;
    }
    if (media == nullptr) {
        ++unknownOf(entry.system).entries;
    }
    SystemCoverage& system = systemOf(entry.system);
    for (const CoveragePopulation population : kCoveragePopulations) {
        if (!inPopulation(entry, population)) {
            continue;
        }
        CoverageCounts& counts = system[population];
        ++counts.entries;
        if (media == nullptr) {
            ++counts.unknown;
            continue;
        }
        for (std::size_t i = 0; i < kMediaKindCount; ++i) {
            counts.withKind[i] += media->kinds[i].present ? 1u : 0u;
        }
        counts.withAny += media->any() ? 1u : 0u;
    }
}

void CoverageBuilder::noteUnknown(const SystemName& system, std::string_view reason) {
    Unknown& unknown = unknownOf(system);
    if (unknown.reason.empty()) {
        unknown.reason = reason;
    }
}

MediaCoverage CoverageBuilder::finish() const {
    MediaCoverage coverage;
    coverage.systems = m_systems;
    for (const SystemCoverage& system : coverage.systems) {
        for (const CoveragePopulation population : kCoveragePopulations) {
            coverage.total[population].add(system[population]);
        }
    }
    for (const Unknown& unknown : m_unknown) {
        if (unknown.entries == 0) {
            continue; // a reason noted for no member
        }
        coverage.issues.push_back(
            Issue{IssueSeverity::Warning, std::string(kCoverageIssueMediaUnknown), unknown.system.str(), std::string(),
                  fmt::format("the media of {} entr{} of {} are not known, and no share counts them{}{}",
                              unknown.entries, unknown.entries == 1 ? "y" : "ies", unknown.system.str(),
                              unknown.reason.empty() ? "" : ": ", unknown.reason)});
    }
    return coverage;
}

Expected<MediaCoverage> measureCoverage(const LibrarySnapshot& snapshot, IMediaResolver& resolver,
                                        const std::optional<SystemName>& system) {
    CoverageBuilder builder;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (system && entry.system != *system) {
            continue;
        }
        const bool member = std::any_of(kCoveragePopulations.begin(), kCoveragePopulations.end(),
                                        [&](CoveragePopulation population) { return inPopulation(entry, population); });
        if (!member) {
            continue;
        }
        if (entry.key.empty()) {
            // Outside its system folder: ES-DE skips the entry, so it has no media.
            const EntryMedia none;
            builder.add(entry, &none);
            continue;
        }
        const bool directory = entry.file.has_value() && entry.file->directory;
        const auto media = resolver.presence(entry.system, entry.key, directory);
        if (media) {
            builder.add(entry, &*media);
            continue;
        }
        if (media.error().code != ErrorCode::Unavailable) {
            return propagate(media.error());
        }
        builder.add(entry, nullptr);
        builder.noteUnknown(entry.system, media.error().message);
    }
    return builder.finish();
}

} // namespace rv::catalog
