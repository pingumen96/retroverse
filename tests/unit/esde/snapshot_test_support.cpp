#include "snapshot_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <utility>

#include <rv/catalog/entry.hpp>
#include <rv/core/tree_walk.hpp>

namespace rv::test::esde {

Fixture materialize(std::string_view name) {
    auto fixture = Fixture::materialize(name);
    INFO((fixture ? std::string("materialized") : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    return std::move(*fixture);
}

catalog::LibraryProfile profileOf(const Fixture& fixture) {
    catalog::LibraryProfile profile;
    profile.esdeHome = fixture.path("ES-DE");
    profile.installFolder = fixture.path("Programs/ES-DE");
    return profile;
}

catalog::LibrarySnapshot loadSnapshot(catalog::ILibrarySource& source, const catalog::LibraryProfile& profile,
                                      const catalog::LibraryLoadOptions& options) {
    auto snapshot = source.load(profile, options);
    INFO((snapshot ? std::string("loaded") : snapshot.error().toString()));
    REQUIRE(snapshot.has_value());
    return std::move(*snapshot);
}

std::ostream& operator<<(std::ostream& out, const StateCounts& counts) {
    return out << "{entries " << counts.entries << ", games " << counts.games << ", missing " << counts.missing
               << ", not a game " << counts.notAGame << ", members " << counts.members << ", folders "
               << counts.folders << ", outside " << counts.outside << "}";
}

StateCounts countsOf(const catalog::LibrarySnapshot& snapshot, std::string_view system) {
    StateCounts counts;
    for (const catalog::CatalogEntry& entry : snapshot.entries) {
        if (entry.system.str() != system) {
            continue;
        }
        ++counts.entries;
        switch (entry.state) {
        case catalog::EntryState::Game:
            ++counts.games;
            break;
        case catalog::EntryState::MissingFile:
            ++counts.missing;
            break;
        case catalog::EntryState::NotAGame:
            ++counts.notAGame;
            break;
        case catalog::EntryState::DiscMember:
            ++counts.members;
            break;
        case catalog::EntryState::Folder:
            ++counts.folders;
            break;
        case catalog::EntryState::OutsideSystemFolder:
            ++counts.outside;
            break;
        }
    }
    return counts;
}

const catalog::CatalogEntry* findEntry(const catalog::LibrarySnapshot& snapshot, std::string_view system,
                                       std::string_view relativePath) {
    for (const catalog::CatalogEntry& entry : snapshot.entries) {
        if (entry.system.str() == system && entry.key.relativePath() == relativePath) {
            return &entry;
        }
    }
    return nullptr;
}

const catalog::CatalogEntry& entryAt(const catalog::LibrarySnapshot& snapshot, std::string_view system,
                                     std::string_view relativePath) {
    const catalog::CatalogEntry* entry = findEntry(snapshot, system, relativePath);
    INFO(std::string(system) + "/" + std::string(relativePath));
    REQUIRE(entry != nullptr);
    return *entry;
}

std::vector<std::string> pathsIn(const catalog::LibrarySnapshot& snapshot, std::string_view system,
                                 catalog::EntryState state) {
    std::vector<std::string> paths;
    for (const catalog::CatalogEntry& entry : snapshot.entries) {
        if (entry.system.str() == system && entry.state == state) {
            paths.push_back(entry.key.empty() ? entry.gamelistPath : entry.key.relativePath());
        }
    }
    return paths;
}

std::vector<std::string> codesOf(const std::vector<catalog::Issue>& issues) {
    std::vector<std::string> codes;
    for (const catalog::Issue& issue : issues) {
        codes.push_back(issue.code);
    }
    return codes;
}

std::vector<catalog::Issue> issuesOf(const std::vector<catalog::Issue>& issues, std::string_view code) {
    std::vector<catalog::Issue> found;
    for (const catalog::Issue& issue : issues) {
        if (issue.code == code) {
            found.push_back(issue);
        }
    }
    return found;
}

TreeState treeState(const std::filesystem::path& root) {
    core::TreeWalkOptions options;
    options.followLinks = false;
    const auto entries = core::listTree(root, options);
    REQUIRE(entries.has_value());
    TreeState state;
    for (const core::TreeEntry& entry : *entries) {
        // NTFS updates the times a folder's listing shows for its sub-folders lazily: only files are compared by time.
        const bool file = entry.kind == core::TreeEntryKind::File;
        state.emplace_back(entry.relativePath, static_cast<int>(entry.kind), entry.size,
                           file ? entry.lastWriteTime.time_since_epoch().count() : 0);
    }
    return state;
}

} // namespace rv::test::esde
