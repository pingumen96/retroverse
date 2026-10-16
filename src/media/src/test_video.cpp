#include <rv/media/test_video.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/assets/test_patterns.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include "ffmpeg_support.hpp"

namespace rv::media {
namespace {

constexpr int kMinTestVideoSide = 16;
constexpr int kMaxTestVideoSide = 4096;
constexpr int kMaxTestVideoFrames = 36000;

// --- The frame counter: kFrameCounterBits blocks, most significant first, in a row along the top
// ------------------------

/// Side of a counter block for a frame `width` pixels wide; blocks start half a block from the top-left corner.
double counterBlockSide(int width) noexcept {
    return static_cast<double>(width) / 20.0;
}

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// An RGBA8 picture being drawn, rows top to bottom.
class Canvas {
public:
    Canvas(int width, int height)
        : m_width(width), m_height(height),
          m_pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, std::uint8_t{255}) {}

    void fill(int x0, int y0, int x1, int y1, Rgb color) noexcept {
        x0 = std::clamp(x0, 0, m_width);
        x1 = std::clamp(x1, 0, m_width);
        y0 = std::clamp(y0, 0, m_height);
        y1 = std::clamp(y1, 0, m_height);
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* pixel = row(y) + static_cast<std::size_t>(x0) * 4u;
            for (int x = x0; x < x1; ++x, pixel += 4) {
                pixel[0] = color.r;
                pixel[1] = color.g;
                pixel[2] = color.b;
                pixel[3] = 255;
            }
        }
    }

    /// Copies an RGBA image with its top-left corner at (x, y), clipped.
    void blit(const assets::ImageData& image, int x, int y) noexcept {
        if (!image.isValid() || image.channels != 4) {
            return;
        }
        for (int sy = 0; sy < image.height; ++sy) {
            const int ty = y + sy;
            if (ty < 0 || ty >= m_height) {
                continue;
            }
            const int x0 = std::max(x, 0);
            const int x1 = std::min(x + image.width, m_width);
            if (x0 >= x1) {
                continue;
            }
            const std::uint8_t* source = image.pixels.data() + (static_cast<std::size_t>(sy) * image.rowBytes()) +
                                         static_cast<std::size_t>(x0 - x) * 4u;
            std::memcpy(row(ty) + static_cast<std::size_t>(x0) * 4u, source, static_cast<std::size_t>(x1 - x0) * 4u);
        }
    }

    [[nodiscard]] std::uint8_t* row(int y) noexcept {
        return m_pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) * 4u;
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return m_pixels.data(); }
    [[nodiscard]] int stride() const noexcept { return m_width * 4; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

private:
    int m_width;
    int m_height;
    std::vector<std::uint8_t> m_pixels;
};

/// Bars of the background, in the style §3 palette and a few saturated test colors.
constexpr std::array<Rgb, 8> kBarColors = {
    Rgb{0x49, 0x30, 0x24}, Rgb{0x76, 0x51, 0x3A}, Rgb{0x4C, 0x8B, 0xC9}, Rgb{0x6E, 0xAF, 0x93},
    Rgb{0xB3, 0x4D, 0xCE}, Rgb{0xFF, 0xC8, 0x8A}, Rgb{0x68, 0x3D, 0x42}, Rgb{0x25, 0x2A, 0x2E},
};

/// Triangle wave: 0 .. span .. 0 with period 2 * span.
int bounce(int value, int span) noexcept {
    if (span <= 0) {
        return 0;
    }
    const int phase = value % (2 * span);
    return phase <= span ? phase : 2 * span - phase;
}

void drawTestFrame(Canvas& canvas, const TestVideoSpec& spec, int frameIndex) {
    const int width = canvas.width();
    const int height = canvas.height();
    // Background bars, scrolling one bar width every two seconds.
    const int barCount = static_cast<int>(kBarColors.size());
    const int barWidth = std::max(1, width / barCount);
    const int scroll = (frameIndex * barWidth) / std::max(1, 2 * spec.frameRate);
    const int seedShift = static_cast<int>(spec.seed % kBarColors.size());
    for (int bar = -1; bar <= barCount; ++bar) {
        const int x0 = bar * barWidth + scroll % barWidth;
        const int shifted = bar - scroll / barWidth + seedShift;
        const int colorIndex = ((shifted % barCount) + barCount) % barCount;
        canvas.fill(x0, 0, x0 + barWidth, height, kBarColors[static_cast<std::size_t>(colorIndex)]);
    }
    // A white box bouncing across the picture: motion for the encoder and for the eye.
    const int box = std::max(4, height / 6);
    const int boxX = bounce(frameIndex * std::max(1, width / 90), width - box);
    const int boxY = height / 3 + bounce(frameIndex * std::max(1, height / 120), height / 3 - box / 2);
    canvas.fill(boxX, boxY, boxX + box, boxY + box, Rgb{240, 240, 240});

    // The counter blocks on a dark band.
    const double block = counterBlockSide(width);
    canvas.fill(0, 0, static_cast<int>(std::ceil(block * (kFrameCounterBits + 1))),
                static_cast<int>(std::ceil(block * 2.0)), Rgb{20, 20, 20});
    const auto counter = static_cast<std::uint32_t>(frameIndex) & ((1u << kFrameCounterBits) - 1u);
    for (int bit = 0; bit < kFrameCounterBits; ++bit) {
        const bool one = ((counter >> (kFrameCounterBits - 1 - bit)) & 1u) != 0;
        const int x0 = static_cast<int>(std::lround(block * (0.5 + bit)));
        const int x1 = static_cast<int>(std::lround(block * (1.5 + bit)));
        const int y0 = static_cast<int>(std::lround(block * 0.5));
        const int y1 = static_cast<int>(std::lround(block * 1.5));
        canvas.fill(x0, y0, x1, y1, one ? Rgb{255, 255, 255} : Rgb{0, 0, 0});
    }

    // The frame number in large digits, after the clip's letter.
    const int textWidth = std::max(8, width / 2);
    const int textHeight = std::max(8, height / 5);
    const char letter = static_cast<char>('A' + static_cast<int>(spec.seed % 26u));
    const assets::ImageData lettering =
        assets::makeSignLettering(textWidth, textHeight, fmt::format("{} {:04}", letter, frameIndex));
    canvas.blit(lettering, (width - textWidth) / 2, height - textHeight - height / 20);
}

struct OutputContextDeleter {
    void operator()(AVFormatContext* context) const noexcept {
        if (context == nullptr) {
            return;
        }
        if (context->pb != nullptr && (context->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
    }
};
using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

Status ffmpegError(std::string_view what, int code, const std::filesystem::path& path) {
    return makeError(ErrorCode::IoError, fmt::format("{}: {}", what, detail::errorText(code)), core::pathToUtf8(path));
}

/// Sends `frame` (null to flush) to the encoder and writes every packet it returns.
Status encodeAndWrite(AVCodecContext& encoder, AVFormatContext& output, const AVStream& stream, const AVFrame* frame,
                      AVPacket& packet, const std::filesystem::path& path) {
    int result = avcodec_send_frame(&encoder, frame);
    if (result < 0) {
        return ffmpegError("cannot encode a frame", result, path);
    }
    for (;;) {
        result = avcodec_receive_packet(&encoder, &packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return ok();
        }
        if (result < 0) {
            return ffmpegError("cannot encode a frame", result, path);
        }
        av_packet_rescale_ts(&packet, encoder.time_base, stream.time_base);
        packet.stream_index = stream.index;
        result = av_interleaved_write_frame(&output, &packet); // takes the packet's data
        if (result < 0) {
            return ffmpegError("cannot write a packet", result, path);
        }
    }
}

struct ContainerChoice {
    const char* format = nullptr;
    bool mp4 = false;
    bool avi = false;
};

std::optional<ContainerChoice> containerFor(const std::filesystem::path& path) {
    const std::string extension = core::toLowerAscii(core::pathToUtf8(path.extension()));
    if (extension == ".mp4") {
        return ContainerChoice{"mp4", true, false};
    }
    if (extension == ".mkv") {
        return ContainerChoice{"matroska", false, false};
    }
    if (extension == ".avi") {
        return ContainerChoice{"avi", false, true};
    }
    return std::nullopt;
}

Status writeTestVideoUnchecked(const std::filesystem::path& path, const TestVideoSpec& spec,
                               const ContainerChoice& container) {
    const std::string context = core::pathToUtf8(path);
    const AVCodecID codecId = spec.codec == TestVideoCodec::Mjpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_MPEG4;
    if (container.mp4 && spec.codec == TestVideoCodec::Mjpeg) {
        return makeError(ErrorCode::Unsupported, "Motion JPEG test clips go into .mkv or .avi files", context);
    }
    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (codec == nullptr) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("this FFmpeg build has no {} encoder", toString(spec.codec)), context);
    }

    const std::string url = detail::fileUrl(path);
    AVFormatContext* rawOutput = nullptr;
    int result = avformat_alloc_output_context2(&rawOutput, nullptr, container.format, url.c_str());
    if (result < 0 || rawOutput == nullptr) {
        return ffmpegError("cannot create the container", result, path);
    }
    OutputContextPtr output(rawOutput);
    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    detail::CodecContextPtr encoder(avcodec_alloc_context3(codec));
    detail::FramePtr picture(av_frame_alloc());
    detail::PacketPtr packet(av_packet_alloc());
    if (stream == nullptr || !encoder || !picture || !packet) {
        return makeError(ErrorCode::IoError, "out of memory", context);
    }

    const bool fullRange = spec.codec == TestVideoCodec::Mjpeg;
    const AVColorSpace matrix = spec.height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    encoder->width = spec.width;
    encoder->height = spec.height;
    encoder->time_base = AVRational{1, spec.frameRate};
    encoder->framerate = AVRational{spec.frameRate, 1};
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    encoder->colorspace = matrix;
    encoder->color_primaries = spec.height >= 720 ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
    encoder->color_trc = spec.height >= 720 ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
    encoder->gop_size = std::max(1, spec.keyFrameInterval);
    // AVI keeps no presentation timestamps: no reordering there.
    encoder->max_b_frames = spec.codec == TestVideoCodec::Mpeg4 && !container.avi ? std::max(0, spec.bFrames) : 0;
    encoder->flags |= AV_CODEC_FLAG_QSCALE;
    encoder->global_quality = FF_QP2LAMBDA * spec.quantizer;
    encoder->thread_count = 1;
    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    result = avcodec_open2(encoder.get(), codec, nullptr);
    if (result < 0) {
        return ffmpegError(fmt::format("cannot open the {} encoder", toString(spec.codec)), result, path);
    }
    result = avcodec_parameters_from_context(stream->codecpar, encoder.get());
    if (result < 0) {
        return ffmpegError("cannot describe the stream", result, path);
    }
    stream->time_base = encoder->time_base;
    stream->avg_frame_rate = encoder->framerate;

    result = avio_open(&output->pb, url.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) {
        return ffmpegError("cannot create the file", result, path);
    }
    AVDictionary* muxerOptions = nullptr;
    if (container.mp4) {
        av_dict_set(&muxerOptions, "movflags", "+faststart", 0);
    }
    result = avformat_write_header(output.get(), &muxerOptions);
    av_dict_free(&muxerOptions);
    if (result < 0) {
        return ffmpegError("cannot write the container header", result, path);
    }

    picture->format = encoder->pix_fmt;
    picture->width = spec.width;
    picture->height = spec.height;
    picture->color_range = encoder->color_range;
    picture->colorspace = matrix;
    result = av_frame_get_buffer(picture.get(), 0);
    if (result < 0) {
        return ffmpegError("cannot allocate a frame", result, path);
    }
    detail::SwsPtr toYuv(sws_getContext(spec.width, spec.height, AV_PIX_FMT_RGBA, spec.width, spec.height,
                                        AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!toYuv) {
        return makeError(ErrorCode::IoError, "cannot convert RGBA to YUV 4:2:0", context);
    }
    const int swsMatrix = matrix == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    sws_setColorspaceDetails(toYuv.get(), sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(swsMatrix),
                             fullRange ? 1 : 0, 0, 1 << 16, 1 << 16);

    Canvas canvas(spec.width, spec.height);
    const int frameCount = std::max(1, static_cast<int>(std::lround(spec.durationSeconds * spec.frameRate)));
    for (int index = 0; index < frameCount; ++index) {
        drawTestFrame(canvas, spec, index);
        result = av_frame_make_writable(picture.get());
        if (result < 0) {
            return ffmpegError("cannot reuse a frame", result, path);
        }
        const std::array<const std::uint8_t*, 4> source{canvas.data(), nullptr, nullptr, nullptr};
        const std::array<int, 4> sourceStride{canvas.stride(), 0, 0, 0};
        sws_scale(toYuv.get(), source.data(), sourceStride.data(), 0, spec.height, picture->data, picture->linesize);
        picture->pts = index;
        if (auto written = encodeAndWrite(*encoder, *output, *stream, picture.get(), *packet, path); !written) {
            return written;
        }
    }
    if (auto flushed = encodeAndWrite(*encoder, *output, *stream, nullptr, *packet, path); !flushed) {
        return flushed;
    }
    result = av_write_trailer(output.get());
    if (result < 0) {
        return ffmpegError("cannot finish the container", result, path);
    }
    return ok();
}

/// Reads a whole file of at most 1 GiB.
Expected<std::vector<std::byte>> readFile(const std::filesystem::path& path) {
    return core::readBinaryFile(path);
}

} // namespace

std::string_view toString(TestVideoCodec codec) noexcept {
    return codec == TestVideoCodec::Mjpeg ? "mjpeg" : "mpeg4";
}

Status writeTestVideo(const std::filesystem::path& path, const TestVideoSpec& spec) {
    const std::string context = core::pathToUtf8(path);
    if (spec.width < kMinTestVideoSide || spec.height < kMinTestVideoSide || spec.width > kMaxTestVideoSide ||
        spec.height > kMaxTestVideoSide || spec.width % 2 != 0 || spec.height % 2 != 0) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("test video size {}x{} must be even and within [{}, {}]", spec.width, spec.height,
                                     kMinTestVideoSide, kMaxTestVideoSide),
                         context);
    }
    if (spec.frameRate < 1 || spec.frameRate > 240) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("frame rate {} is outside [1, 240]", spec.frameRate),
                         context);
    }
    if (!std::isfinite(spec.durationSeconds) || spec.durationSeconds <= 0.0 ||
        spec.durationSeconds * spec.frameRate > kMaxTestVideoFrames) {
        return makeError(
            ErrorCode::InvalidArgument,
            fmt::format("duration {} s is not in (0, {} frames]", spec.durationSeconds, kMaxTestVideoFrames), context);
    }
    if (spec.quantizer < 2 || spec.quantizer > 31) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("quantizer {} is outside [2, 31]", spec.quantizer),
                         context);
    }
    const std::optional<ContainerChoice> container = containerFor(path);
    if (!container) {
        return makeError(ErrorCode::InvalidArgument, "test videos are .mp4, .mkv or .avi files", context);
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    detail::installLogBridge();
    Status written = writeTestVideoUnchecked(path, spec, *container);
    if (!written) {
        std::filesystem::remove(path, ec); // no half-written clip left behind
    }
    return written;
}

std::optional<std::uint32_t> decodeFrameCounter(const VideoFrame& frame) {
    if (!frame.size().valid() || frame.pixels() == nullptr) {
        return std::nullopt;
    }
    const double block = counterBlockSide(frame.width());
    std::uint32_t value = 0;
    for (int bit = 0; bit < kFrameCounterBits; ++bit) {
        // The central half of the block, away from compression ringing at its edges.
        const int x0 = static_cast<int>(std::lround(block * (0.75 + bit)));
        const int x1 = std::max(x0 + 1, static_cast<int>(std::lround(block * (1.25 + bit))));
        const int y0 = static_cast<int>(std::lround(block * 0.75));
        const int y1 = std::max(y0 + 1, static_cast<int>(std::lround(block * 1.25)));
        if (x1 > frame.width() || y1 > frame.height()) {
            return std::nullopt;
        }
        std::uint64_t sum = 0;
        std::uint64_t count = 0;
        for (int y = y0; y < y1; ++y) {
            const std::uint8_t* pixel =
                frame.pixels() + static_cast<std::size_t>(y) * frame.strideBytes() + static_cast<std::size_t>(x0) * 4u;
            for (int x = x0; x < x1; ++x, pixel += 4) {
                sum += (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2]) >> 8; // Rec. 601 luma
                ++count;
            }
        }
        const std::uint64_t luma = sum / count;
        if (luma >= 170) {
            value |= 1u << (kFrameCounterBits - 1 - bit);
        } else if (luma > 85) {
            return std::nullopt;
        }
    }
    return value;
}

Status writeTruncatedCopy(const std::filesystem::path& source, const std::filesystem::path& destination,
                          double keepFraction) {
    auto bytes = readFile(source);
    if (!bytes) {
        return propagate(bytes.error());
    }
    const double fraction = std::isfinite(keepFraction) ? std::clamp(keepFraction, 0.0, 1.0) : 0.0;
    const auto kept = static_cast<std::size_t>(static_cast<double>(bytes->size()) * fraction);
    return core::writeFileAtomic(destination, std::span<const std::byte>(bytes->data(), kept));
}

Status writeCorruptedCopy(const std::filesystem::path& source, const std::filesystem::path& destination,
                          std::uint32_t seed) {
    auto bytes = readFile(source);
    if (!bytes) {
        return propagate(bytes.error());
    }
    constexpr std::size_t kStride = 16 * 1024;
    constexpr std::size_t kDamaged = 512;
    std::uint32_t state = seed * 2654435761u + 0x9E3779B9u;
    const auto next = [&state]() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    const std::size_t begin = bytes->size() * 2 / 5;
    const std::size_t end = bytes->size() * 4 / 5;
    for (std::size_t offset = begin; offset < end; offset += kStride) {
        for (std::size_t index = offset; index < std::min(offset + kDamaged, end); ++index) {
            (*bytes)[index] = static_cast<std::byte>(next() & 0xFFu);
        }
    }
    return core::writeFileAtomic(destination, *bytes);
}

Status writeUnknownCodecCopy(const std::filesystem::path& source, const std::filesystem::path& destination) {
    auto bytes = readFile(source);
    if (!bytes) {
        return propagate(bytes.error());
    }
    // The four-character codes FFmpeg's AVI muxer writes for its native encoders (stream header and bitmap header).
    constexpr std::array<std::string_view, 6> kTags = {"FMP4", "XVID", "DIVX", "DX50", "MP4V", "MJPG"};
    constexpr std::string_view kUnknown = "RVZZ";
    const std::size_t headerEnd = std::min<std::size_t>(bytes->size(), 64 * 1024);
    std::size_t replaced = 0;
    for (std::size_t index = 0; index + 4 <= headerEnd; ++index) {
        for (const std::string_view tag : kTags) {
            if (std::memcmp(bytes->data() + index, tag.data(), 4) == 0) {
                std::memcpy(bytes->data() + index, kUnknown.data(), 4);
                ++replaced;
                break;
            }
        }
    }
    if (replaced == 0) {
        return makeError(ErrorCode::ParseError, "no video codec code found to replace", core::pathToUtf8(source));
    }
    return core::writeFileAtomic(destination, *bytes);
}

Status writeRawFile(const std::filesystem::path& path, std::string_view bytes) {
    return core::writeFileAtomic(path, std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
}

Status writeSilentWav(const std::filesystem::path& path, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 60.0) {
        return makeError(ErrorCode::InvalidArgument, "a silent .wav lasts 0 to 60 s", core::pathToUtf8(path));
    }
    constexpr std::uint32_t kSampleRate = 16000;
    constexpr std::uint16_t kBitsPerSample = 16;
    constexpr std::uint16_t kChannels = 1;
    const auto dataBytes = static_cast<std::uint32_t>(std::lround(seconds * kSampleRate)) * 2u;
    std::vector<std::byte> file;
    file.reserve(44u + dataBytes);
    const auto text = [&file](std::string_view value) {
        for (const char c : value) {
            file.push_back(static_cast<std::byte>(c));
        }
    };
    const auto little = [&file](std::uint32_t value, int bytes) {
        for (int index = 0; index < bytes; ++index) {
            file.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xFFu));
        }
    };
    text("RIFF");
    little(36u + dataBytes, 4);
    text("WAVEfmt ");
    little(16, 4);
    little(1, 2); // PCM
    little(kChannels, 2);
    little(kSampleRate, 4);
    little(kSampleRate * kChannels * kBitsPerSample / 8u, 4);
    little(kChannels * kBitsPerSample / 8u, 2);
    little(kBitsPerSample, 2);
    text("data");
    little(dataBytes, 4);
    file.resize(file.size() + dataBytes, std::byte{0});
    return core::writeFileAtomic(path, file);
}

std::vector<std::string> availableVideoEncoders() {
    std::vector<std::string> names;
    void* iterator = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iterator)) {
        if (av_codec_is_encoder(codec) != 0 && codec->type == AVMEDIA_TYPE_VIDEO && codec->name != nullptr) {
            names.emplace_back(codec->name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace rv::media
