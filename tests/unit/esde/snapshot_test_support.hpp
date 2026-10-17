#pragma once

// What the tests of the ROM scan and the library snapshot (I2 step 7) share: fixtures materialized with their library
// profile, snapshots loaded, entries found and counted by state, and the state of a tree to show that nothing was
// written.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/snapshot.hpp>

#include "support/fixture.hpp"

namespace rv::test::esde {

/// tests/fixtures/<name> materialized (REQUIREd).
[[nodiscard]] Fixture materialize(std::string_view name);

/// The profile of a fixture laid out as the standard one: the ES-DE home in ES-DE/, the install folder in
/// Programs/ES-DE.
[[nodiscard]] catalog::LibraryProfile profileOf(const Fixture& fixture);

/// source.load() REQUIREd to succeed.
[[nodiscard]] catalog::LibrarySnapshot loadSnapshot(catalog::ILibrarySource& source,
                                                    const catalog::LibraryProfile& profile,
                                                    const catalog::LibraryLoadOptions& options = {});

/// A system's entries counted by state.
struct StateCounts {
    std::size_t entries = 0;
    std::size_t games = 0;
    std::size_t missing = 0;
    std::size_t notAGame = 0;
    std::size_t members = 0;
    std::size_t folders = 0;
    std::size_t outside = 0;

    bool operator==(const StateCounts&) const = default;
};
std::ostream& operator<<(std::ostream& out, const StateCounts& counts);

[[nodiscard]] StateCounts countsOf(const catalog::LibrarySnapshot& snapshot, std::string_view system);

/// The entry of `system` whose relative path is `relativePath`, or nullptr.
[[nodiscard]] const catalog::CatalogEntry* findEntry(const catalog::LibrarySnapshot& snapshot, std::string_view system,
                                                     std::string_view relativePath);
/// findEntry() REQUIREd to find it.
[[nodiscard]] const catalog::CatalogEntry& entryAt(const catalog::LibrarySnapshot& snapshot, std::string_view system,
                                                   std::string_view relativePath);

/// The entries of `system` in a state, as "relative path" (or the gamelist path for those outside the folder).
[[nodiscard]] std::vector<std::string> pathsIn(const catalog::LibrarySnapshot& snapshot, std::string_view system,
                                               catalog::EntryState state);

/// The codes of issues, in order.
[[nodiscard]] std::vector<std::string> codesOf(const std::vector<catalog::Issue>& issues);
/// The issues of a code.
[[nodiscard]] std::vector<catalog::Issue> issuesOf(const std::vector<catalog::Issue>& issues, std::string_view code);

/// Every file and folder under `root`, each file with its size and last write time (links not followed): what a write
/// would change.
using TreeState = std::vector<std::tuple<std::string, int, std::uint64_t, std::int64_t>>;
[[nodiscard]] TreeState treeState(const std::filesystem::path& root);

} // namespace rv::test::esde
