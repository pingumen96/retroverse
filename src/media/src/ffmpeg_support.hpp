#pragma once

// FFmpeg glue private to rv_media: RAII owners of FFmpeg objects, error text, the log bridge to spdlog. FFmpeg headers
// are included here and in rv_media's sources only, never in its public headers.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace rv::media::detail {

struct InputFormatDeleter {
    void operator()(AVFormatContext* context) const noexcept { avformat_close_input(&context); }
};
/// An opened input (avformat_open_input).
using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct SwsDeleter {
    void operator()(SwsContext* context) const noexcept { sws_freeContext(context); }
};
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

struct DictionaryDeleter {
    void operator()(AVDictionary* dictionary) const noexcept { av_dict_free(&dictionary); }
};

/// FFmpeg's description of an error code ("Invalid data found when processing input"), with the code.
[[nodiscard]] std::string errorText(int code);

/// URL FFmpeg opens for a file: "file:" + the UTF-8 path, so that the file protocol is used whatever the name looks
/// like (FFmpeg converts UTF-8 to UTF-16 on Windows: non-ASCII names work).
[[nodiscard]] std::string fileUrl(const std::filesystem::path& path);

/// Routes FFmpeg's log to spdlog once per process: panics and fatal errors as errors, errors as warnings, warnings as
/// debug messages, the rest dropped. Thread-safe.
void installLogBridge();

/// Seconds of `timestamp` in `timeBase`.
[[nodiscard]] inline double toSeconds(std::int64_t timestamp, AVRational timeBase) noexcept {
    return static_cast<double>(timestamp) * av_q2d(timeBase);
}

} // namespace rv::media::detail
