#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The coverage and library screens' content from what the catalog worker read (I7 step 8, ADR-0018 §14): the figures in
// text, as they show, the systems worst first, the import's issues grouped by kind; and what runs now (the refresh, the
// quick-hash pass, the file watcher's modes) for the library screen.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/app/catalog_worker.hpp>
#include <rv/app/directory_data.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/ui/coverage_model.hpp>
#include <rv/ui/library_model.hpp>

namespace rv::app {

/// What the coverage screen shows of `list`: per population, the systems with at least one member, worst first (the
/// mean share of covers, 3D boxes, screenshots, marquees and videos, lowest first; then the full name, the name), each
/// kind's share in text ("90,9"), with and missing counts, a share under half marked low; the clips known to fail.
[[nodiscard]] ui::CoverageContent coverageContentOf(const CoverageList& list);

/// The kind of an import issue's code, the library screen's group: "guard", "rom-root", "system-folder",
/// "system-config", "gamelist", "gamelist-entry", "rom-entry", "key-collision", "disc", "media-root", "media", "mame",
/// "other".
[[nodiscard]] std::string_view issueGroupOf(std::string_view code) noexcept;

/// What the library screen shows of `status`: its folders, the last import (its day through `localDate`), the issues
/// grouped by kind (the worst severity first, then the most numerous), the systems the guard holds, the open review rows
/// (a candidate's GameId only when its file is present), the quick hashes' counts.
[[nodiscard]] ui::LibraryContent libraryContentOf(const LibraryStatus& status, const LocalDate& localDate);

/// What the file watcher says of its roots: whether it is on, and each root's mode by its name.
struct WatchStatus {
    bool watching = false;
    std::vector<std::pair<std::string, std::string>> modes;

    bool operator==(const WatchStatus&) const = default;
};

/// What runs now, for the library screen.
[[nodiscard]] ui::LibraryLive libraryLiveOf(const RefreshState& refresh, const QuickHashState& hash,
                                            const WatchStatus& watch);

} // namespace rv::app
