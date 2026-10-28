#pragma once

// Private to rv_cli_core: `media --check-videos` (I5 step 7, ADR-0016 §8), cmd_media_videos.cpp, called by runMedia().

#include <rv/catalog/ids.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

#include <optional>

namespace rv::cli {

/// The jobs of a check by default, and at most.
inline constexpr int kDefaultVideoCheckJobs = 2;
inline constexpr int kMaxVideoCheckJobs = 8;

/// media --check-videos [--system <name>] [--jobs N] [--json]: checks the clips of the media root's videos folders (one
/// system's with `system`) that have no verdict for their file as it is now, on `jobs` threads at below-normal
/// priority, merges the verdicts into <user data>/cache/media/videos.json as it goes (an interrupted run resumes), and
/// reports the verdicts per kind and system, the failures and the near misses. Exit codes: 0; 1 when the media root or
/// a videos folder could not be read or a clip's check did not finish; 3 when the verdicts cannot be written.
[[nodiscard]] ExitCode checkVideos(CommandContext& context, Output& out, const esde::ResolvedRoot& mediaRoot,
                                   const esde::SystemsConfiguration& systems,
                                   const std::optional<catalog::SystemName>& system, int jobs, bool json);

} // namespace rv::cli
