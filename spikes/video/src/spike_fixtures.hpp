#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/media/test_video.hpp>

namespace rv::spike {

/// The files --generate-fixtures writes.
struct FixtureSet {
    /// Test clips, one per seed (A, B, ...): what the spike plays by default.
    std::vector<std::filesystem::path> clips;
    /// The good clip A followed by the damaged files of specification SC-08: a path with no file, an empty file, the
    /// clip cut in half, the clip with a corrupt middle, a text file named .mp4, an audio-only .wav, an .avi whose
    /// codec no decoder knows.
    std::vector<std::filesystem::path> errorCases;
};

/// Writes `clipCount` clips of `spec` (seeds 0, 1, ...; container from `extension`) into `directory`, and with
/// `errorCases` the damaged files. Clips whose file already exists are kept unless `regenerate` (their names hold the
/// spec). Logs what it writes and how long it takes.
[[nodiscard]] Expected<FixtureSet> prepareFixtures(const std::filesystem::path& directory,
                                                   const media::TestVideoSpec& spec, const std::string& extension,
                                                   int clipCount, bool errorCases, bool regenerate);

} // namespace rv::spike
