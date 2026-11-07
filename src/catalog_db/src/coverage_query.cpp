// SqliteCatalogQuery::mediaCoverage, ::importIssues and ::quickHashCounts (I7 step 8; ADR-0018 §14): what the menu's
// Copertura media and Libreria read of the catalog, each one statement on the caller's connection (one snapshot), so that
// opening either scans no folder. The coverage groups every Game row of idx_games with the kinds idx_media holds for it
// (a bit per kind: idx_media's primary key is (game, kind), so its rows are read once, in order) and counts them in C++
// into catalog::MediaCoverage's Games and ScrapedGames populations: the same counts catalog::measureCoverage() makes of
// the snapshot the last import read, for those two populations (tests/unit/catalog_db/test_coverage_query.cpp). The
// statements are prepared per call, as the room snapshot's are.

#include <rv/catalog_db/catalog_query.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog_db/database.hpp>

namespace rv::catalog_db {
namespace {

using catalog::CoveragePopulation;

constexpr std::string_view kCoverageSql =
    "WITH m AS (SELECT game, sum(1 << kind) AS kinds FROM idx_media GROUP BY game) "
    "SELECT g.system, g.gamelist_path <> '', coalesce(m.kinds, 0) "
    "FROM idx_games AS g LEFT JOIN m ON m.game = g.id "
    "WHERE g.library = ?1 AND g.state = ?2 "
    "ORDER BY g.system";

/// Counts one member with the kinds of `mask` into `counts`.
void count(catalog::CoverageCounts& counts, std::uint32_t mask) {
    ++counts.entries;
    for (std::size_t i = 0; i < catalog::kMediaKindCount; ++i) {
        counts.withKind[i] += (mask >> i) & 1u;
    }
    counts.withAny += mask != 0 ? 1u : 0u;
}

} // namespace

Expected<catalog::MediaCoverage> SqliteCatalogQuery::mediaCoverage(const catalog::LibraryId& library) {
    auto statement = connection().prepare(kCoverageSql);
    if (!statement) {
        return propagate(statement.error());
    }
    statement->bindText(1, library.str()).bindText(2, catalog::toString(catalog::EntryState::Game));
    constexpr std::uint32_t kKnownKinds = (1u << catalog::kMediaKindCount) - 1u;
    catalog::MediaCoverage coverage;
    {
        const ScopedReset reset(*statement);
        while (true) {
            const auto stepped = statement->step();
            if (!stepped) {
                return propagate(stepped.error());
            }
            if (!*stepped) {
                break;
            }
            const std::string_view systemText = statement->columnText(0);
            if (coverage.systems.empty() || coverage.systems.back().system.str() != systemText) {
                auto system = catalog::SystemName::parse(systemText);
                if (!system) {
                    return makeError(ErrorCode::ParseError, "the catalog database holds a system name that is none",
                                     "idx_games");
                }
                coverage.systems.push_back(catalog::SystemCoverage{std::move(*system), {}});
            }
            const std::int64_t kinds = statement->columnInt(2);
            if (kinds < 0 || (static_cast<std::uint64_t>(kinds) & ~std::uint64_t{kKnownKinds}) != 0) {
                return makeError(ErrorCode::ParseError, fmt::format("an unknown media kind among {:#x}", kinds),
                                 "idx_media");
            }
            const auto mask = static_cast<std::uint32_t>(kinds);
            catalog::SystemCoverage& system = coverage.systems.back();
            count(system[CoveragePopulation::Games], mask);
            if (statement->columnInt(1) != 0) {
                count(system[CoveragePopulation::ScrapedGames], mask);
            }
        }
    }
    // SQLite orders text by its bytes, as SystemName compares (ordinal): MediaCoverage::find() relies on it.
    std::sort(coverage.systems.begin(), coverage.systems.end(),
              [](const catalog::SystemCoverage& a, const catalog::SystemCoverage& b) { return a.system < b.system; });
    for (const catalog::SystemCoverage& system : coverage.systems) {
        for (const CoveragePopulation population : catalog::kCoveragePopulations) {
            coverage.total[population].add(system[population]);
        }
    }
    return coverage;
}

Expected<std::vector<catalog::Issue>> SqliteCatalogQuery::importIssues(const catalog::LibraryId& library) {
    auto statement = connection().prepare(
        "SELECT severity, code, system, file, reason FROM idx_import_issues WHERE library = ?1 ORDER BY id");
    if (!statement) {
        return propagate(statement.error());
    }
    statement->bindText(1, library.str());
    std::vector<catalog::Issue> issues;
    const ScopedReset reset(*statement);
    while (true) {
        const auto stepped = statement->step();
        if (!stepped) {
            return propagate(stepped.error());
        }
        if (!*stepped) {
            break;
        }
        const auto severity = catalog::parseIssueSeverity(statement->columnText(0));
        if (!severity) {
            return makeError(ErrorCode::ParseError, "the catalog database holds an issue of no severity",
                             "idx_import_issues");
        }
        issues.push_back(catalog::Issue{*severity, std::string(statement->columnText(1)),
                                        std::string(statement->columnText(2)), std::string(statement->columnText(3)),
                                        std::string(statement->columnText(4))});
    }
    return issues;
}

Expected<QuickHashCounts> SqliteCatalogQuery::quickHashCounts(const catalog::LibraryId& library) {
    auto statement = connection().prepare(
        "SELECT count(*), coalesce(sum(quick_hash IS NOT NULL AND quick_hash_size = file_size AND quick_hash_time = "
        "file_time), 0) FROM rv_game_identities WHERE library = ?1 AND present = 1 AND is_directory = 0");
    if (!statement) {
        return propagate(statement.error());
    }
    statement->bindText(1, library.str());
    const ScopedReset reset(*statement);
    const auto stepped = statement->step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    QuickHashCounts counts;
    if (*stepped) {
        counts.files = static_cast<std::size_t>(std::max<std::int64_t>(statement->columnInt(0), 0));
        counts.current = static_cast<std::size_t>(std::max<std::int64_t>(statement->columnInt(1), 0));
    }
    return counts;
}

} // namespace rv::catalog_db
