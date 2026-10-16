#include "ffmpeg_support.hpp"

#include <array>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string_view>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>
#include <rv/media/video_decoder.hpp>

namespace rv::media {
namespace detail {
namespace {

/// spdlog level of an FFmpeg log level; spdlog::level::off for the levels that are dropped.
spdlog::level::level_enum spdlogLevel(int level) noexcept {
    if (level <= AV_LOG_FATAL) {
        return spdlog::level::err;
    }
    if (level <= AV_LOG_ERROR) {
        return spdlog::level::warn;
    }
    if (level <= AV_LOG_WARNING) {
        return spdlog::level::debug;
    }
    return spdlog::level::off;
}

void logBridge(void* avClass, int level, const char* format, std::va_list arguments) {
    const spdlog::level::level_enum target = spdlogLevel(level);
    if (target == spdlog::level::off || !spdlog::should_log(target)) {
        return;
    }
    // FFmpeg may deliver a line in pieces; each piece is logged on its own, prefixed with the component when it starts
    // a line.
    thread_local int printPrefix = 1;
    std::array<char, 1024> line{};
    av_log_format_line2(avClass, level, format, arguments, line.data(), static_cast<int>(line.size()), &printPrefix);
    std::string_view text(line.data(), std::strlen(line.data()));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (!text.empty()) {
        spdlog::log(target, "FFmpeg: {}", text);
    }
}

std::string versionText(unsigned version) {
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFFu, version & 0xFFu);
}

} // namespace

std::string errorText(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return fmt::format("FFmpeg error {}", code);
    }
    return fmt::format("{} ({})", buffer.data(), code);
}

std::string fileUrl(const std::filesystem::path& path) {
    return "file:" + core::pathToUtf8(path);
}

void installLogBridge() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(&logBridge);
    });
}

} // namespace detail

std::string ffmpegVersionInfo() {
    return fmt::format("FFmpeg {} (libavformat {}, libavcodec {}, libswscale {}, libavutil {})", av_version_info(),
                       detail::versionText(avformat_version()), detail::versionText(avcodec_version()),
                       detail::versionText(swscale_version()), detail::versionText(avutil_version()));
}

} // namespace rv::media
