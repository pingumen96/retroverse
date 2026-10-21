#include "screen_service.hpp"

#include <rv/app/run_report.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/cpu_timers.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/media/video_decoder.hpp>
#include <rv/media/video_playback.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/video_texture.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

/// How long a frame waits for the frozen frame of --video-freeze-pts, and a lockstep run for a still picture.
constexpr std::chrono::seconds kFreezeWait{5};
constexpr std::chrono::seconds kStillWait{5};
/// Polling interval of those waits and of the stop before a launch.
constexpr std::chrono::milliseconds kPollInterval{1};

/// Decoders of every screen service alive (playing or still ending): never more than one (NFR-07's single decoder of
/// I3, ADR-0012 §7).
std::atomic<int> g_liveDecoders{0};

/// The decoder's failure as the run report names it, like the error codes: "file-not-found", "corrupt-data".
std::string failureName(media::DecodeFailure failure) {
    std::string name(media::toString(failure));
    std::replace(name.begin(), name.end(), ' ', '-');
    return name;
}

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// A still picture decoded and fitted on the worker, for the main thread to upload.
struct StillResult {
    std::uint64_t token = 0;
    Expected<assets::ImageData> picture = assets::ImageData{};
};

/// Where the worker leaves its result.
struct StillSlot {
    std::mutex mutex;
    std::optional<StillResult> result;
};

/// Reads (shared, as files other programs rewrite), decodes and fits a still to the screen's size. The error names the
/// game's picture, not its path (the user's folders).
Expected<assets::ImageData> loadStill(const std::filesystem::path& path, std::string_view context) {
    auto bytes = core::readFileShared(path, assets::kMaxImageFileBytes);
    if (!bytes) {
        return propagate(bytes.error());
    }
    auto image = assets::decodeImage(std::as_bytes(std::span(bytes->data(), bytes->size())), 0, context);
    if (!image) {
        return propagate(image.error());
    }
    auto fitted = assets::fitCover(*image, ScreenService::kStillSize.x, ScreenService::kStillSize.y);
    if (!fitted) {
        return propagate(fitted.error());
    }
    return std::move(fitted->picture);
}

} // namespace

std::optional<HeroScreen> findHeroScreen(const scene::SceneDescription& description, const SceneInstance& instance,
                                         const gl::MaterialRegistry& materials,
                                         const std::optional<std::string>& idPrefix) {
    if (idPrefix && idPrefix->empty()) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < description.objects.size(); ++index) {
        const scene::SceneObject& object = description.objects[index];
        if (object.source != scene::SceneObject::Source::Prefab || object.kind != "crt" ||
            !object.texturesByRole.contains("screen") || index >= instance.objectBounds().size()) {
            continue;
        }
        if (idPrefix && !object.id.starts_with(*idPrefix)) {
            continue;
        }
        // SceneInstance registers the screen's picture instance as "<object id>/screen", "@<w>x<h>" for a pattern.
        const std::string key = object.id + "/screen";
        for (std::uint32_t handle = 0; handle < materials.size(); ++handle) {
            const gl::GpuMaterial& material = materials.material(scene::MaterialHandle{handle});
            if (material.key != key && !material.key.starts_with(key + "@")) {
                continue;
            }
            if (material.textureSources[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] == nullptr) {
                continue;
            }
            HeroScreen screen;
            screen.objectId = object.id;
            screen.material = scene::MaterialHandle{handle};
            const std::span<const glm::mat4> transforms = object.instanceTransforms();
            screen.bounds = instance.objectBounds()[index].transformed(transforms.front());
            return screen;
        }
    }
    return std::nullopt;
}

struct ScreenService::Impl {
    Impl(HeroScreen heroScreen, gl::MaterialRegistry& registry, std::vector<ScreenGame> games,
         ScreenServiceOptions serviceOptions)
        : screen(std::move(heroScreen)), materials(registry), policy(std::move(games)), options(serviceOptions),
          pool(core::WorkerPoolOptions{
              .threads = 1, .capacity = 8, .belowNormalPriority = true, .name = "rv-screen-stills"}),
          stillSlot(std::make_shared<StillSlot>()) {}

    HeroScreen screen;
    gl::MaterialRegistry& materials;
    ScreenPolicy policy;
    ScreenServiceOptions options;

    // The one decoder: the clip playing, or the one whose thread is still ending after a stop (never both at once with
    // a thread: a new clip opens once `draining` is gone).
    std::unique_ptr<media::VideoPlayback> playback;
    std::unique_ptr<media::VideoPlayback> draining;
    bool playbackPaused = false;
    gl::VideoTexture videoTexture;
    bool videoTextureFailed = false;

    // The clip playing: when its decoder started, its first frame, its frames and their timestamps.
    Clock::time_point clipStarted;
    bool clipFrameShown = false;
    double clipDuration = 0.0;
    std::optional<double> firstFrameMs;
    std::uint64_t clipFrames = 0;
    std::optional<double> firstPts;
    std::optional<double> lastPts;
    std::optional<double> firstUploadAt;
    double lastUploadAt = 0.0;
    bool frozenShown = false;

    // Still pictures: the one shown (for the generation it was loaded for) and the load in flight.
    core::WorkerPool pool;
    core::JobGeneration stillJobs;
    std::shared_ptr<StillSlot> stillSlot;
    std::uint64_t stillToken = 0;
    bool stillLoading = false;
    std::optional<ScreenPicture> loadingPicture;
    gl::Texture2D still;

    // What the material shows and the generation the screen follows.
    GLuint appliedOverride = 0;
    ScreenPicture shownPicture = ScreenPicture::IdlePattern;
    std::uint64_t followedGeneration = 0;
    bool followed = false;
    bool reopenPending = false;

    // The run report's counters, and the phase and decoder state when the run ended (shutdown()).
    VideoRecord counters;
    std::optional<std::string> endPhase;
    std::optional<std::string> endDecoderState;

    [[nodiscard]] std::string decoderStateName() const {
        return playback ? std::string(media::toString(playback->decoder().state())) : std::string("none");
    }

    // --- decoder -----------------------------------------------------------------------------------------------

    void releaseDecoder(std::unique_ptr<media::VideoPlayback>& slot) {
        if (slot) {
            slot.reset();
            g_liveDecoders.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /// Stops the clip playing: its decoder ends on its own thread, waited for by reapDraining() (or the launch's stop).
    void stopPlayback() {
        if (!playback) {
            return;
        }
        // A decoder opens only once the previous one's thread ended (openDecoder), so nothing drains now.
        assert(!draining);
        playback->decoder().stop();
        releaseDecoder(draining);
        draining = std::move(playback);
        clipFrameShown = false;
    }

    /// Forgets a stopped decoder once its thread ended.
    void reapDraining() {
        if (draining && !draining->decoder().threadRunning()) {
            releaseDecoder(draining);
        }
    }

    [[nodiscard]] std::uint64_t decoderThreads() const {
        std::uint64_t threads = 0;
        for (const auto* slot : {&playback, &draining}) {
            if (*slot && (*slot)->decoder().threadRunning()) {
                ++threads;
            }
        }
        return threads;
    }

    void addFallback(ScreenPicture picture, std::string reason) {
        spdlog::warn("Hero CRT: the {} of game {} cannot be shown ({}); the next picture of the chain is shown",
                     toString(picture), policy.targetId(), reason);
        counters.fallbacks.push_back(
            VideoFallbackRecord{policy.targetId(), std::string(toString(picture)), std::move(reason)});
    }

    void openDecoder(double nowSeconds) {
        if (draining || playback) {
            return; // the previous clip's thread is still ending: never two decoders
        }
        if (g_liveDecoders.load(std::memory_order_relaxed) != 0) {
            // Another screen service holds the one decoder (none does in I3): refused, never two.
            assert(false && "never two video decoders");
            spdlog::error("Hero CRT: a second video decoder was refused (one decoder at a time)");
            policy.videoFailed();
            return;
        }
        media::DecoderSettings settings;
        settings.maxHeight = kMaxVideoRows;
        settings.loop = true;
        settings.startPaused = options.freezePtsSeconds.has_value();
        settings.name = "hero-crt";
        auto decoder = media::VideoDecoder::start(policy.targetMedia().video, std::move(settings));
        if (!decoder) {
            addFallback(ScreenPicture::Video, std::string(toString(decoder.error().code)));
            policy.videoFailed();
            return;
        }
        g_liveDecoders.fetch_add(1, std::memory_order_relaxed);
        ++counters.decodersStarted;
        playback = std::make_unique<media::VideoPlayback>(std::move(*decoder));
        playbackPaused = false;
        clipStarted = Clock::now();
        clipFrameShown = false;
        clipDuration = 0.0;
        firstFrameMs.reset();
        clipFrames = 0;
        firstPts.reset();
        lastPts.reset();
        firstUploadAt.reset();
        frozenShown = false;
        const std::optional<double> resume = policy.takeResumePosition();
        if (std::exchange(reopenPending, false) && !counters.launches.empty()) {
            counters.launches.back().reopened = true;
            counters.launches.back().reopenedAtSeconds = resume;
        }
        if (options.freezePtsSeconds) {
            playback->pause(nowSeconds);
            playback->seek(*options.freezePtsSeconds);
            playbackPaused = true;
        } else if (resume) {
            playback->seek(*resume);
        }
        spdlog::info("Hero CRT: the video of game {} opens{}", policy.targetId(),
                     options.freezePtsSeconds
                         ? fmt::format(", held at {:g} s (--video-freeze-pts)", *options.freezePtsSeconds)
                     : resume ? fmt::format(" at {:.3f} s, where it stopped for the launch", *resume)
                              : std::string());
        spdlog::debug("Hero CRT: video file {}", core::pathToUtf8Lossy(policy.targetMedia().video));
    }

    void upload(const media::VideoFrame& frame, double nowSeconds) {
        const glm::ivec2 size(frame.width(), frame.height());
        // The texture and its ring are made once, outside the section: it times the uploads (the budget's p99).
        if (!videoTexture.valid()) {
            auto created = gl::VideoTexture::create(size, "HeroCrt.Video");
            if (!created) {
                spdlog::error("Hero CRT: no video texture: {}", created.error().toString());
                videoTextureFailed = true;
                return;
            }
            videoTexture = std::move(*created);
        }
        Expected<gl::VideoUpload> uploaded = gl::VideoUpload::RingBusy;
        {
            const core::CpuSectionScope timing(core::cpu_section::kVideoUpload);
            uploaded = videoTexture.upload(frame.bytes(), size, frame.strideBytes());
        }
        if (!uploaded) {
            spdlog::error("Hero CRT: a video frame was not uploaded: {}", uploaded.error().toString());
            playback->releaseFrame();
            return;
        }
        if (*uploaded == gl::VideoUpload::RingBusy) {
            ++counters.ringBusy; // kept pending: retried at the next frame
            return;
        }
        playback->releaseFrame();
        ++counters.framesUploaded;
        ++clipFrames;
        if (const std::optional<media::FrameTiming>& timing = playback->clock().current()) {
            if (!firstPts) {
                firstPts = timing->ptsSeconds;
            }
            lastPts = timing->ptsSeconds;
        }
        if (!firstUploadAt) {
            firstUploadAt = nowSeconds;
        }
        lastUploadAt = nowSeconds;
        if (options.freezePtsSeconds) {
            frozenShown = true;
        }
        if (!clipFrameShown) {
            clipFrameShown = true;
            firstFrameMs = millisecondsSince(clipStarted);
            if (const std::optional<media::StreamInfo> info = playback->decoder().streamInfo()) {
                clipDuration = info->durationSeconds;
            }
            policy.videoShowing();
            spdlog::info("Hero CRT: the first frame of game {}'s video after {:.1f} ms", policy.targetId(),
                         *firstFrameMs);
        }
    }

    /// The playback's frame of `nowSeconds`, uploaded when it changed.
    void pump(double nowSeconds) {
        const media::PlaybackUpdate update = playback->update(nowSeconds);
        if (update.frame != nullptr && !videoTextureFailed) {
            upload(*update.frame, nowSeconds);
        } else if (update.frame != nullptr) {
            playback->releaseFrame();
        }
    }

    void updatePlayback(double nowSeconds) {
        if (!options.freezePtsSeconds) {
            const bool wantPaused = policy.decoderPaused();
            if (wantPaused != playbackPaused) {
                if (wantPaused) {
                    playback->pause(nowSeconds);
                    ++counters.pauses;
                } else {
                    playback->resume(nowSeconds);
                }
                playbackPaused = wantPaused;
            }
        }
        pump(nowSeconds);
        if (options.freezePtsSeconds && !frozenShown && !videoTextureFailed) {
            // The frame at the frozen time is what the screenshots show: it is waited for (the decoder delivers it a
            // few milliseconds after the seek).
            const Clock::time_point deadline = Clock::now() + kFreezeWait;
            while (!frozenShown && !playback->failed() && !videoTextureFailed && Clock::now() < deadline) {
                std::this_thread::sleep_for(kPollInterval);
                pump(nowSeconds);
            }
            if (!frozenShown && !playback->failed()) {
                spdlog::warn("Hero CRT: the frame at {:g} s did not arrive within {} s", *options.freezePtsSeconds,
                             kFreezeWait.count());
            }
        }
        if (playback->failed() || videoTextureFailed) {
            const media::DecodeFailure failure = playback->decoder().failure();
            addFallback(ScreenPicture::Video,
                        videoTextureFailed ? std::string(toString(ErrorCode::GraphicsError)) : failureName(failure));
            policy.videoFailed();
            stopPlayback();
        }
    }

    // --- stills ------------------------------------------------------------------------------------------------

    void requestStill() {
        if (stillLoading) {
            return;
        }
        const std::optional<ScreenPicture> wanted = policy.wantedStill();
        if (!wanted) {
            return;
        }
        const std::filesystem::path path = *mediaPath(policy.targetMedia(), *wanted);
        const std::uint64_t token = ++stillToken;
        const std::string context = fmt::format("{} of game {}", toString(*wanted), policy.targetId());
        auto job = [slot = stillSlot, token, path, context](const core::JobContext& job) {
            StillResult result{token, assets::ImageData{}};
            if (job.cancelled()) {
                result.picture = makeError(ErrorCode::Cancelled, "cancelled", context);
            } else {
                result.picture = loadStill(path, context);
            }
            const std::scoped_lock lock(slot->mutex);
            slot->result = std::move(result);
        };
        core::JobOptions jobOptions;
        jobOptions.generation = &stillJobs;
        if (auto submitted = pool.submit(std::move(job), jobOptions); !submitted) {
            addFallback(*wanted, std::string(toString(submitted.error().code)));
            policy.stillFailed();
            return;
        }
        stillLoading = true;
        loadingPicture = wanted;
    }

    /// The result of the load in flight, if it arrived (a stale one is dropped).
    std::optional<StillResult> takeStill() {
        const std::scoped_lock lock(stillSlot->mutex);
        if (!stillSlot->result) {
            return std::nullopt;
        }
        std::optional<StillResult> result = std::exchange(stillSlot->result, std::nullopt);
        if (result->token != stillToken) {
            return std::nullopt;
        }
        return result;
    }

    void updateStills() {
        // A failed still asks for the next one at once: a chain of missing files falls through within the frame.
        for (int guard = 0; guard < 4; ++guard) {
            requestStill();
            if (!stillLoading) {
                return;
            }
            std::optional<StillResult> result = takeStill();
            if (!result && options.lockstep) {
                const Clock::time_point deadline = Clock::now() + kStillWait;
                while (!result && Clock::now() < deadline) {
                    std::this_thread::sleep_for(kPollInterval);
                    result = takeStill();
                }
            }
            if (!result) {
                return;
            }
            stillLoading = false;
            const ScreenPicture picture = loadingPicture.value_or(ScreenPicture::Cover);
            if (result->picture) {
                gl::TextureDesc desc;
                desc.colorSpace = gl::ColorSpace::Srgb;
                desc.mipmaps = true;
                desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
                desc.memoryKind = gl::GpuMemoryKind::HeroTextures;
                auto texture = gl::Texture2D::create(*result->picture, desc, "HeroCrt.Still");
                if (texture) {
                    still = std::move(*texture);
                    policy.stillReady();
                    return;
                }
                addFallback(picture, std::string(toString(texture.error().code)));
            } else {
                spdlog::debug("Hero CRT: {}", result->picture.error().toString());
                addFallback(picture, std::string(toString(result->picture.error().code)));
            }
            policy.stillFailed();
        }
    }

    // --- the screen ----------------------------------------------------------------------------------------------

    /// A new target: the previous clip stops, its still goes, the new one's first still loads.
    void followTarget() {
        if (followed && policy.targetGeneration() == followedGeneration) {
            return;
        }
        followed = true;
        followedGeneration = policy.targetGeneration();
        if (!policy.targetId().empty()) {
            ++counters.targets;
        }
        stopPlayback();
        pool.cancel(stillJobs);
        ++stillToken; // a result of the previous target is dropped
        stillLoading = false;
        loadingPicture.reset();
        still = gl::Texture2D();
        if (policy.targetId().empty()) {
            spdlog::info("Hero CRT: no game to show, the idle pattern");
        } else {
            spdlog::info("Hero CRT: game {} ({})", policy.targetId(),
                         policy.selected() ? "selected" : "the start choice");
            if (!policy.targetMedia().video.empty() && !isPlayableVideoFile(policy.targetMedia().video)) {
                addFallback(ScreenPicture::Video, "not-playable");
            }
        }
    }

    void applyPicture() {
        const ScreenPicture picture = policy.picture();
        GLuint texture = 0;
        if (picture == ScreenPicture::Video) {
            texture = videoTexture.textureId();
        } else if (picture != ScreenPicture::IdlePattern) {
            texture = still.id();
        }
        if (texture == appliedOverride && picture == shownPicture) {
            return;
        }
        if (auto set = materials.setTextureOverride(screen.material, gl::MaterialTextureSlot::BaseColor, texture);
            !set) {
            spdlog::error("Hero CRT: the screen's picture cannot be set: {}", set.error().toString());
            return;
        }
        if (picture != shownPicture) {
            spdlog::info("Hero CRT shows {}{}", toString(picture),
                         policy.targetId().empty() ? std::string() : " of game " + policy.targetId());
        }
        appliedOverride = texture;
        shownPicture = picture;
    }

    void update(double nowSeconds, const scene::Camera& camera, std::optional<std::string_view> hovered) {
        if (policy.phase() == ScreenPhase::Quit) {
            return;
        }
        policy.hover(hovered, nowSeconds);
        followTarget();
        const bool visible = camera.frustum().intersects(screen.bounds);
        policy.setVisible(visible);
        reapDraining();
        if (policy.wantsDecoder() && !playback) {
            openDecoder(nowSeconds);
        }
        if (playback) {
            updatePlayback(nowSeconds);
        }
        updateStills();
        applyPicture();
        counters.maxDecoderThreads = std::max(counters.maxDecoderThreads, decoderThreads());
    }

    /// Seconds into the clip shown (timestamps keep growing across loops: taken modulo its duration).
    [[nodiscard]] std::optional<double> clipPosition() const {
        if (!clipFrameShown || !lastPts) {
            return std::nullopt;
        }
        return clipDuration > 0.0 ? std::fmod(*lastPts, clipDuration) : *lastPts;
    }

    /// Stops the decoder and waits for its thread, at most kScreenStopBoundSeconds.
    VideoLaunchRecord stopAndJoin() {
        VideoLaunchRecord stop;
        stop.decoderRan = playback != nullptr || draining != nullptr;
        stop.positionSeconds = clipPosition();
        stopPlayback();
        const Clock::time_point start = Clock::now();
        const auto bound = std::chrono::duration<double>(kScreenStopBoundSeconds);
        while (draining && draining->decoder().threadRunning() && Clock::now() - start < bound) {
            std::this_thread::sleep_for(kPollInterval);
        }
        stop.stopMs = stop.decoderRan ? millisecondsSince(start) : 0.0;
        stop.threadsRunning = decoderThreads();
        reapDraining();
        return stop;
    }

    void launchStarting() {
        if (policy.phase() != ScreenPhase::Running) {
            return;
        }
        VideoLaunchRecord stop = stopAndJoin();
        policy.launchStarting(stop.positionSeconds);
        if (stop.threadsRunning > 0) {
            spdlog::warn("Hero CRT: the video decoder's thread did not end within {:g} s; the launch goes on (it ends "
                         "on its own)",
                         kScreenStopBoundSeconds);
        } else if (stop.decoderRan) {
            spdlog::info("Hero CRT: the video decoder stopped for the launch in {:.1f} ms", stop.stopMs);
        }
        counters.launches.push_back(stop);
    }

    void launchEnded() {
        if (policy.phase() != ScreenPhase::Launched) {
            return;
        }
        policy.launchEnded();
        reopenPending = true;
    }

    void shutdown() {
        if (policy.phase() == ScreenPhase::Quit && !playback && !draining) {
            return;
        }
        // What the run ended with, for the report: the phase (launched when quitting during a game) and the decoder.
        endPhase = std::string(toString(policy.phase()));
        endDecoderState = decoderStateName();
        policy.quit();
        stopPlayback();
        // Every stopped decoder's thread (ADR-0004: at shutdown, never from the render loop).
        media::waitForStoppedDecoders();
        reapDraining();
        counters.joinedAtExit = decoderThreads() == 0;
        pool.shutdown();
    }
};

ScreenService::ScreenService(HeroScreen screen, gl::MaterialRegistry& materials, std::vector<ScreenGame> games,
                             ScreenServiceOptions options)
    : m_impl(std::make_unique<Impl>(std::move(screen), materials, std::move(games), options)) {
    spdlog::info(
        "Hero CRT: the screen of '{}', {} game(s) with media{}", m_impl->screen.objectId, m_impl->policy.games().size(),
        options.freezePtsSeconds ? fmt::format(", videos held at {:g} s", *options.freezePtsSeconds) : std::string());
}

ScreenService::~ScreenService() {
    if (!m_impl) {
        return;
    }
    m_impl->shutdown();
    // The textures go with the service: the material draws its own picture again.
    if (m_impl->appliedOverride != 0) {
        static_cast<void>(
            m_impl->materials.setTextureOverride(m_impl->screen.material, gl::MaterialTextureSlot::BaseColor, 0));
    }
    m_impl->releaseDecoder(m_impl->playback);
    m_impl->releaseDecoder(m_impl->draining);
}

void ScreenService::setGames(std::vector<ScreenGame> games) {
    m_impl->policy.setGames(std::move(games));
}

void ScreenService::update(double nowSeconds, const scene::Camera& camera, std::optional<std::string_view> hovered) {
    m_impl->update(nowSeconds, camera, hovered);
}

void ScreenService::launchStarting() {
    m_impl->launchStarting();
}

void ScreenService::launchEnded() {
    m_impl->launchEnded();
}

void ScreenService::shutdown() {
    m_impl->shutdown();
}

void ScreenService::record(VideoRecord& record) const {
    const Impl& s = *m_impl;
    record = s.counters;
    record.bound = true;
    record.screen = s.screen.objectId;
    record.gameId = s.policy.targetId();
    record.picture = std::string(toString(s.shownPicture));
    record.phase = s.endPhase.value_or(std::string(toString(s.policy.phase())));
    record.decoderState = s.endDecoderState.value_or(s.decoderStateName());
    record.freezePtsSeconds = s.options.freezePtsSeconds;
    record.frozenFrameShown = s.frozenShown;
    record.selections = s.policy.selections();
    record.firstFrameMs = s.firstFrameMs;
    record.clipFramesUploaded = s.clipFrames;
    record.firstPtsSeconds = s.firstPts;
    record.lastPtsSeconds = s.lastPts;
    record.clipUploadSeconds = s.firstUploadAt ? s.lastUploadAt - *s.firstUploadAt : 0.0;
    record.videoBytes = s.videoTexture.valid() ? s.videoTexture.gpuBytes() : 0;
    record.stillBytes = s.still.valid() ? s.still.estimatedBytes() : 0;
}

std::vector<std::string> ScreenService::statusLines() const {
    const Impl& s = *m_impl;
    std::vector<std::string> lines;
    lines.push_back(fmt::format("Hero CRT '{}': {}{}", s.screen.objectId, toString(s.shownPicture),
                                s.policy.targetId().empty() ? std::string() : " of " + s.policy.targetId()));
    lines.push_back(fmt::format("Decoder: {}{}, {} thread(s); {} started, at most {} at once", s.decoderStateName(),
                                s.policy.visible() ? "" : " (out of view)", s.decoderThreads(),
                                s.counters.decodersStarted, s.counters.maxDecoderThreads));
    lines.push_back(fmt::format("Frames uploaded: {} ({} of the clip, ring busy {}); first frame {}",
                                s.counters.framesUploaded, s.clipFrames, s.counters.ringBusy,
                                s.firstFrameMs ? fmt::format("{:.1f} ms", *s.firstFrameMs) : std::string("-")));
    if (s.lastPts) {
        lines.push_back(fmt::format("Clip time: {:.3f} s", *s.lastPts));
    }
    lines.push_back(
        fmt::format("Fallbacks: {}; launches: {}", s.counters.fallbacks.size(), s.counters.launches.size()));
    return lines;
}

} // namespace rv::app
