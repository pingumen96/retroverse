#include "spike_fixtures.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>

namespace rv::spike {
namespace {

std::string clipName(const media::TestVideoSpec& spec, const std::string& extension) {
    return fmt::format("clip_{}_{}x{}_{}fps_{:g}s_{}{}", static_cast<char>('A' + static_cast<int>(spec.seed % 26u)),
                       spec.width, spec.height, spec.frameRate, spec.durationSeconds, media::toString(spec.codec),
                       extension);
}

Status writeClipIfNeeded(const std::filesystem::path& path, const media::TestVideoSpec& spec, bool regenerate) {
    std::error_code ec;
    if (!regenerate && std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0) {
        spdlog::info("Fixture kept: {}", core::pathToUtf8(path));
        return ok();
    }
    const auto start = std::chrono::steady_clock::now();
    if (auto written = media::writeTestVideo(path, spec); !written) {
        return written;
    }
    spdlog::info("Fixture written in {:.2f} s: {} ({:.1f} MiB)",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                 core::pathToUtf8(path), static_cast<double>(std::filesystem::file_size(path, ec)) / 1048576.0);
    return ok();
}

} // namespace

Expected<FixtureSet> prepareFixtures(const std::filesystem::path& directory, const media::TestVideoSpec& spec,
                                     const std::string& extension, int clipCount, bool errorCases, bool regenerate) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return makeError(ErrorCode::IoError, fmt::format("cannot create the directory: {}", ec.message()),
                         core::pathToUtf8(directory));
    }
    FixtureSet set;
    for (int index = 0; index < std::max(clipCount, 1); ++index) {
        media::TestVideoSpec clip = spec;
        clip.seed = static_cast<std::uint32_t>(index);
        const std::filesystem::path path = directory / clipName(clip, extension);
        if (auto written = writeClipIfNeeded(path, clip, regenerate); !written) {
            return propagate(written.error());
        }
        set.clips.push_back(path);
    }
    if (!errorCases) {
        return set;
    }

    // The damaged files of SC-08, from clip A (an .mp4 with its index first: the truncated copy opens, then ends
    // mid-stream) and a small .avi (whose codec code can be replaced).
    media::TestVideoSpec source = spec;
    source.seed = 0;
    source.codec = media::TestVideoCodec::Mpeg4; // the .mp4 container takes MPEG-4 only
    const std::filesystem::path sourceMp4 = directory / clipName(source, ".mp4");
    if (auto written = writeClipIfNeeded(sourceMp4, source, regenerate); !written) {
        return propagate(written.error());
    }
    media::TestVideoSpec small = source;
    small.width = 320;
    small.height = 240;
    small.durationSeconds = 2.0;
    const std::filesystem::path sourceAvi = directory / clipName(small, ".avi");
    if (auto written = writeClipIfNeeded(sourceAvi, small, regenerate); !written) {
        return propagate(written.error());
    }

    const std::filesystem::path missing = directory / "error_missing.mp4";
    std::filesystem::remove(missing, ec);
    const std::filesystem::path empty = directory / "error_empty.mp4";
    const std::filesystem::path truncated = directory / "error_truncated.mp4";
    const std::filesystem::path corrupt = directory / "error_corrupt.mp4";
    const std::filesystem::path text = directory / "error_not_a_video.mp4";
    const std::filesystem::path audio = directory / "error_audio_only.wav";
    const std::filesystem::path unknown = directory / "error_unknown_codec.avi";
    for (const Status& status :
         {media::writeRawFile(empty, {}), media::writeTruncatedCopy(sourceMp4, truncated, 0.5),
          media::writeCorruptedCopy(sourceMp4, corrupt, 1),
          media::writeRawFile(text, "Retroverse test fixture: a text file with a video's name.\n"),
          media::writeSilentWav(audio, 2.0), media::writeUnknownCodecCopy(sourceAvi, unknown)}) {
        if (!status) {
            return propagate(status.error());
        }
    }
    spdlog::info("Damaged fixtures written in {}", core::pathToUtf8(directory));
    set.errorCases = {sourceMp4, missing, empty, truncated, corrupt, text, audio, unknown};
    return set;
}

} // namespace rv::spike
