#pragma once

// Private to rv_cli_core (I2 steps 7, 8 and 11a): what `import`, `media` and `show` share: the library
// <user data>/config/library.json records, its snapshot as the import reads it (esde::EsdeLibrarySource: ES-DE's
// settings and systems, every system's ROM folder under the roots library.json overrides listed with ES-DE's rules, its
// gamelist merged in, the disc groups), its media root, and the media files of an indexed entry derived from the
// presence and extension the catalog stores (paths are never stored, ADR-0008 §5).

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/cli/command.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/paths.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/library/library_preview.hpp>

#include "esde_text.hpp"

namespace rv::cli {

/// Reads the library <user data>/config/library.json records (library create writes it; the dispatcher has kept the
/// user data directory out of its folders). On failure the message goes to the context's stderr after `name` and
/// `failure` says how the command exits: UsageError without a library.json, RuntimeFailure when it cannot be read.
inline std::optional<catalog::LibraryProfile> readRecordedLibrary(CommandContext& context, std::string_view name,
                                                                  ExitCode& failure) {
    const std::filesystem::path file = context.paths.configDir() / esde::kLibraryFileName;
    auto profile = esde::readLibraryFile(file);
    if (profile) {
        return std::move(*profile);
    }
    if (profile.error().code == ErrorCode::NotFound) {
        context.err.writeLine(fmt::format("{}: no library yet ({} does not exist): run 'retroverse-cli library create'",
                                          name, pathText(file)));
        failure = ExitCode::UsageError;
    } else {
        context.err.writeLine(fmt::format("{}: {}", name, profile.error().toString()));
        failure = ExitCode::RuntimeFailure;
    }
    return std::nullopt;
}

/// A library read into a snapshot, and what the read did (library::previewLibrary()).
using LibraryRead = library::LibraryPreview;

/// library::previewLibrary() of `profile`; only the system named `system` when one is given.
inline Expected<LibraryRead> readLibrarySnapshot(const catalog::LibraryProfile& profile,
                                                 const std::optional<std::string>& system) {
    library::PreviewOptions options;
    options.system = system;
    return library::previewLibrary(profile, options);
}

using library::mediaRootOf;

/// Per media kind, the file of an indexed entry's media of that kind, derived under `mediaRoot`
/// (esde::EsdeMediaResolver::path(), without looking at the disk) from the presence and extension the catalog stores;
/// nothing for a kind that is not present, or when the root is not known.
inline std::array<std::optional<std::filesystem::path>, catalog::kMediaKindCount>
derivedMediaPaths(const catalog::IndexedEntry& indexed, const esde::ResolvedRoot& mediaRoot) {
    std::array<std::optional<std::filesystem::path>, catalog::kMediaKindCount> paths{};
    const esde::EsdeMediaResolver resolver(mediaRoot);
    const catalog::CatalogEntry& entry = indexed.entry;
    // A folder's media keep its whole name (catalog::mediaStem()): a Folder entry, a game that is a folder.
    const bool directory = entry.file ? entry.file->directory : entry.state == catalog::EntryState::Folder;
    for (std::size_t i = 0; i < catalog::kMediaKindCount; ++i) {
        const catalog::MediaPresence& media = indexed.media.kinds[i];
        if (!media.present) {
            continue;
        }
        if (auto path = resolver.path(entry.system, entry.key, directory, catalog::kMediaKinds[i], media)) {
            paths[i] = std::move(*path);
        }
    }
    return paths;
}

} // namespace rv::cli
