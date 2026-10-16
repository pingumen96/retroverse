#include "media_test_support.hpp"

#include <atomic>
#include <string>
#include <system_error>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

namespace rv::test {
namespace {

std::atomic<unsigned> g_directoryCounter{0};

} // namespace

MediaTestDir::MediaTestDir() {
    const std::filesystem::path root = core::executablePath().parent_path() / "test-media";
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::filesystem::path candidate =
            root / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(static_cast<long long>(ticks)) + "-" +
                    std::to_string(g_directoryCounter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (std::filesystem::create_directory(candidate, ec) && !ec) {
            m_path = candidate;
            return;
        }
    }
    FAIL("cannot create a directory for the media fixtures under " << core::pathToUtf8(root));
}

MediaTestDir::~MediaTestDir() {
    // A stopped decoder's thread ends on its own and may still hold a file of the directory.
    media::waitForStoppedDecoders();
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
}

std::filesystem::path MediaTestDir::file(std::string_view relativeUtf8) const {
    return m_path / core::pathFromUtf8(relativeUtf8);
}

media::TestVideoSpec smallClip(int width, int height, int frameRate, int frames) {
    media::TestVideoSpec spec;
    spec.width = width;
    spec.height = height;
    spec.frameRate = frameRate;
    spec.durationSeconds = static_cast<double>(frames) / static_cast<double>(frameRate);
    spec.keyFrameInterval = 5;
    return spec;
}

std::filesystem::path writeClip(const MediaTestDir& dir, std::string_view name, const media::TestVideoSpec& spec) {
    const std::filesystem::path path = dir.file(name);
    const Status written = media::writeTestVideo(path, spec);
    INFO(core::pathToUtf8(path));
    REQUIRE(written.has_value());
    return path;
}

bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::vector<DecodedFrame> collectFrames(media::VideoDecoder& decoder, std::size_t count,
                                        std::chrono::milliseconds timeout) {
    std::vector<DecodedFrame> frames;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (frames.size() < count && std::chrono::steady_clock::now() < deadline) {
        media::PooledFrame frame = decoder.frames().pop();
        if (frame) {
            frames.push_back(DecodedFrame{frame->timing, frame->serial, frame->loop, frame->size(),
                                          media::decodeFrameCounter(*frame)});
            continue;
        }
        const media::DecoderState state = decoder.state();
        if ((state == media::DecoderState::Failed || state == media::DecoderState::Ended ||
             state == media::DecoderState::Stopped) &&
            decoder.frames().empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frames;
}

std::unique_ptr<media::VideoDecoder> startDecoder(const std::filesystem::path& path, media::DecoderSettings settings) {
    auto decoder = media::VideoDecoder::start(path, std::move(settings));
    REQUIRE(decoder.has_value());
    return std::move(*decoder);
}

} // namespace rv::test
