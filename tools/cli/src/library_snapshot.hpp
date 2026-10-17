#pragma once

// Private to rv_cli_core (I2 steps 7 and 8): what `import --dry-run` and `media --coverage` share: the library
// <user data>/config/library.json records, and its snapshot as the import reads it (esde::EsdeLibrarySource: ES-DE's
// settings and systems, every system's ROM folder under the roots library.json overrides listed with ES-DE's rules, its
// gamelist merged in, the disc groups).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/cli/command.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/paths.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>

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

/// A library read into a snapshot, and what the read did.
struct LibraryRead {
    catalog::LibrarySnapshot snapshot;
    esde::EsdeLoadStatistics statistics;
};

/// Reads the library `profile` describes into a snapshot with esde::EsdeLibrarySource, as the import reads it, but
/// that every cue sheet and playlist that is a game is read (there is no database to take the groups read before
/// from); only the system named `system` when one is given (a partial snapshot, for looking at that system, never for
/// an import). Read-only towards ES-DE. Errors: EsdeLibrarySource::load()'s (InvalidArgument for a system the
/// configuration does not define; NotFound, Unavailable or ParseError when the ES-DE home, its settings or its systems
/// cannot be read).
inline Expected<LibraryRead> readLibrarySnapshot(const catalog::LibraryProfile& profile,
                                                 const std::optional<std::string>& system) {
    esde::EsdeLibrarySourceOptions options;
    if (system) {
        options.systems.push_back(*system);
    }
    esde::EsdeLibrarySource source(std::move(options));
    auto snapshot = source.load(profile, catalog::LibraryLoadOptions{});
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    return LibraryRead{std::move(*snapshot), source.lastStatistics()};
}

} // namespace rv::cli
