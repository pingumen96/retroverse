#include "screen_service.hpp"
#include "screen_verdicts.hpp"

#include <rv/app/audio_policy.hpp>
#include <rv/app/cabinet_screens.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/app/screen_aspect.hpp>
#include <rv/audio/audio_engine.hpp>
#include <rv/audio/audio_ring.hpp>
#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/text_raster.hpp>
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
#include <rv/scene/scene_areas.hpp>
#include <rv/scene/scene_description.hpp>

#include <fmt/format.h>
#include <glm/geometric.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>
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

/// The run report's lists of the games played, the pictures shown and the slots' events keep their first entries only.
constexpr std::size_t kMaxTimelineEntries = 512;
/// A lockstep run's frame waits (at most this long) for its audible clip's sound to be decoded kAudioLeadSeconds past
/// the picture, so that its null mix is the same on every run.
constexpr std::chrono::milliseconds kAudioWait{100};
constexpr double kAudioLeadSeconds = 0.04;
/// A lockstep run's frame waits (at most this long: NFR-06's 2 s from a selection to its first frame) for the first
/// frame of a clip a slot opened for a screen in view, so that the frame it shows on does not depend on the decoder
/// thread's pace (I5 step 4's review).
constexpr std::chrono::seconds kFirstFrameWait{2};
/// An inspected cabinet's screen is held in the hand: this far from the eye (its sound's distance, its rank's).
constexpr double kInHandMeters = 0.5;

/// A decoder's sound into a slot's ring (rv_media knows no mixer: ADR-0016 §9).
class RingSink final : public media::AudioSink {
public:
    explicit RingSink(std::shared_ptr<audio::AudioRing> ring) noexcept : m_ring(std::move(ring)) {}
    void write(std::span<const float> interleaved, double ptsSeconds, std::uint32_t generation) override {
        static_cast<void>(m_ring->write(interleaved, ptsSeconds, generation));
    }

private:
    std::shared_ptr<audio::AudioRing> m_ring;
};

/// Decoders of every screen service alive (playing or still ending): never more than the budget's slots (NFR-07,
/// ADR-0016 §2).
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

/// The scheduler's key of a clip: its file.
std::string clipKey(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
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

/// The title cards' fonts, loaded once on the worker (the first card asked for).
struct CardFonts {
    std::once_flag loaded;
    std::optional<assets::FontFace> title;
    std::optional<assets::FontFace> detail;
    std::optional<Error> error;

    void load(const std::filesystem::path& assetRoot) {
        std::call_once(loaded, [&] {
            auto bold = assets::FontFace::loadBundled(assetRoot, assets::kLabelTitleFontPath);
            auto regular = assets::FontFace::loadBundled(assetRoot, assets::kLabelDetailFontPath);
            if (bold && regular) {
                title = std::move(*bold);
                detail = std::move(*regular);
            } else {
                error = bold ? regular.error() : bold.error();
            }
        });
    }
};

/// Draws `card` at the screen's size (assets::drawTitleCard).
Expected<assets::ImageData> drawCard(CardFonts& fonts, const std::filesystem::path& assetRoot, const ScreenCard& card) {
    fonts.load(assetRoot);
    if (fonts.error) {
        return propagate(*fonts.error);
    }
    assets::FrontLabelRequest request;
    request.title = card.title;
    request.fallbackTitle = card.fallbackTitle;
    request.systemName = card.systemName;
    request.year = card.year;
    request.seed = card.seed;
    request.palette = card.palette;
    auto drawn = assets::drawTitleCard(assets::LabelFonts{&*fonts.title, &*fonts.detail}, request,
                                       ScreenService::kStillSize.x, ScreenService::kStillSize.y);
    if (!drawn) {
        return propagate(drawn.error());
    }
    return std::move(drawn->picture);
}

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

/// The material of `object`'s screen picture in `materials` ("<object id>/screen", "@<w>x<h>" for a pattern) with a
/// base colour texture to override.
std::optional<scene::MaterialHandle> screenMaterial(const scene::SceneObject& object,
                                                    const gl::MaterialRegistry& materials) {
    const std::string key = object.id + "/screen";
    for (std::uint32_t handle = 0; handle < materials.size(); ++handle) {
        const gl::GpuMaterial& material = materials.material(scene::MaterialHandle{handle});
        if (material.key != key && !material.key.starts_with(key + "@")) {
            continue;
        }
        if (material.textureSources[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] == nullptr) {
            continue;
        }
        return scene::MaterialHandle{handle};
    }
    return std::nullopt;
}

} // namespace

ScreenPlaySettings screenPlaySettings(const ScreenServiceOptions& options, const ScreenBinding& screen) {
    const ScreenSettings own = options.video.screen(screen.kind, screen.roomId, screen.objectId);
    ScreenPlaySettings settings;
    settings.autoplay = own.enabled;
    settings.showOnFocus = own.showOnFocus;
    settings.clips = options.video.enabled;
    settings.previewSeconds = own.previewSeconds;
    settings.playlist.mode = own.playlist;
    settings.playlist.roomId = screen.roomId;
    // The hero's seeds keep their screen id across scene ids (I5 step 1); a secondary's are its own.
    settings.playlist.screenId = screen.kind == ScreenKind::Hero ? std::string(kHeroScreenId) : screen.objectId;
    settings.playlist.explicitGames = own.games;
    settings.frozen = options.freezePtsSeconds.has_value();
    settings.startChoice = screen.kind == ScreenKind::Hero;
    return settings;
}

int decoderBudget(const ScreenServiceOptions& options) noexcept {
    return std::clamp(options.maxDecoders.value_or(options.video.maxDecoders), 1, kMaxScreenDecoders);
}

std::vector<ScreenBinding> findScreens(const scene::SceneDescription& description, const SceneInstance& instance,
                                       const gl::MaterialRegistry& materials,
                                       const std::optional<std::string>& idPrefix) {
    std::vector<ScreenBinding> screens;
    if (idPrefix && idPrefix->empty()) {
        return screens;
    }
    const std::string roomId = idPrefix
                                   ? (idPrefix->ends_with('/') ? idPrefix->substr(0, idPrefix->size() - 1) : *idPrefix)
                                   : description.name;
    std::optional<std::size_t> heroArea;
    for (std::size_t index = 0; index < description.objects.size(); ++index) {
        const scene::SceneObject& object = description.objects[index];
        // The CRT, or (the hero only) the arcade's hero cabinet that stands for it (I6 step 6), with its screen's
        // picture.
        const bool crt = object.kind == "crt";
        const bool heroCabinet = screens.empty() && object.kind == kCabinetPrefabKind;
        if (object.source != scene::SceneObject::Source::Prefab || !(crt || heroCabinet) ||
            !object.texturesByRole.contains("screen") || index >= instance.objectBounds().size()) {
            continue;
        }
        if (idPrefix && !object.id.starts_with(*idPrefix)) {
            continue;
        }
        const std::optional<scene::MaterialHandle> material = screenMaterial(object, materials);
        if (!material) {
            continue;
        }
        ScreenBinding screen;
        screen.objectId = object.id;
        screen.roomId = roomId;
        screen.material = *material;
        const std::span<const glm::mat4> transforms = object.instanceTransforms();
        screen.bounds = instance.objectBounds()[index].transformed(transforms.front());
        screen.kind = screens.empty() ? ScreenKind::Hero : ScreenKind::Secondary;
        // I5 step 4: its screen's aspect (ADR-0016 §7); the arcade's hero cabinet keeps the room's playlist whatever the
        // world selects, the focused or inspected cabinet playing its clip itself (§6).
        screen.screenSize = instance.screenSurfaceSize(index);
        screen.followsWorldSelection = !heroCabinet;
        if (!instance.areas().empty()) {
            const std::optional<std::size_t> area = scene::areaAt(instance.areas(), screen.bounds.center());
            if (screens.empty()) {
                heroArea = area;
            } else if (area != heroArea) {
                continue; // a CRT of another area: another room's screen
            }
        }
        screens.push_back(std::move(screen));
    }
    return screens;
}

struct ScreenService::Impl {
    /// One screen and what it shows.
    struct ScreenState {
        ScreenState(ScreenBinding screenBinding, ScreenPolicy screenPolicy)
            : binding(std::move(screenBinding)), policy(std::move(screenPolicy)) {}

        ScreenBinding binding;
        ScreenPolicy policy;

        // Still pictures: the one shown (for the generation it was loaded for) and the load in flight.
        core::JobGeneration stillJobs;
        std::shared_ptr<StillSlot> stillSlot = std::make_shared<StillSlot>();
        std::uint64_t stillToken = 0;
        bool stillLoading = false;
        std::optional<ScreenPicture> loadingPicture;
        gl::Texture2D still;
        /// The previous target's still, kept on the screen while the new target's picture is not ready (I5 step 1).
        gl::Texture2D heldStill;

        // What the material shows and the generation the screen follows.
        GLuint appliedOverride = 0;
        /// A video shown: the slot whose texture the override is, and the clip it played then (a held video ends
        /// once that slot opened another screen's clip or its texture changed).
        std::optional<std::size_t> overrideSlot;
        std::string overrideClip;
        ScreenPicture shownPicture = ScreenPicture::IdlePattern;
        /// The game whose picture is shown (the previous target's while it is held).
        std::string shownGame;
        std::uint64_t followedGeneration = 0;
        bool followed = false;
        /// The previous picture held while the target's is awaited.
        PictureHold held;

        // This frame's request and the scheduler's answer (I5 step 2).
        std::string clip;
        ScreenClass screenClass = ScreenClass::Room;
        bool visible = true;
        double distance = 0.0;
        ScreenAssignment assignment;
        /// The sound: its volume (video.json's kind, room and screen) and its azimuth from the eye this frame.
        double volume = 1.0;
        double azimuth = 0.0;
        /// The policy was told its target's clip shows (videoShowing()), from the slot `lastSlot`.
        bool videoTold = false;
        std::optional<std::size_t> lastSlot;

        // I5 step 4.
        /// An arcade cabinet's screen (setCabinets()): no material of its own, its picture on its room's pooled video
        /// material; held in the hand (inspected).
        bool cabinet = false;
        bool inHand = false;
        /// The rectangle its picture fills (ADR-0016 §7), as last set on its material.
        glm::vec4 rect = kWholeScreen;
        /// The frame it showed when its slot was taken from its clip (still asked for: "needed", "budget"), shown while
        /// that clip waits for a slot again (ADR-0016 §6: the hero cabinet keeps its paused frame), with its game and
        /// rectangle; and whether it shows now.
        gl::Texture2D heldFrame;
        std::string heldFrameGame;
        glm::vec4 heldFrameRect = kWholeScreen;
        bool showingHeldFrame = false;
    };

    /// One decoder slot: its decoder (open, or stopped with its thread still ending) and its texture.
    struct Slot {
        std::unique_ptr<media::VideoPlayback> playback;
        bool draining = false;
        bool paused = false;
        /// The clip (its scheduler key) and the game it opened for.
        std::string clip;
        std::string gameId;
        gl::VideoTexture texture;
        bool textureFailed = false;
        bool failureHandled = false;

        // The clip playing: when its decoder started, its first frame, its frames and their timestamps.
        Clock::time_point started;
        bool frameShown = false;
        double duration = 0.0;
        std::optional<double> firstFrameMs;
        std::uint64_t frames = 0;
        std::optional<double> firstPts;
        std::optional<double> lastPts;
        std::optional<double> firstUploadAt;
        double lastUploadAt = 0.0;
        bool frozenShown = false;
        /// A lockstep run waited for its first frame already (once per opening: a wait that ran out is not repeated).
        bool firstFrameWaited = false;

        /// The frame waiting for its upload (VideoPlayback::update()'s), and the render frame it began to wait.
        const media::VideoFrame* pending = nullptr;
        std::optional<std::uint64_t> pendingSince;

        // The sound (I5 step 5): the slot's mixer source, the clip's ring, whether its stream was recorded.
        std::optional<audio::SourceId> source;
        std::shared_ptr<audio::AudioRing> ring;
        bool audioRecorded = false;

        // The verdicts (I5 step 7): the clip's media, whether it opened at its start (a clip played through from
        // there is playable), whether its first pass was looked at.
        ScreenMedia media;
        bool fromStart = false;
        bool passChecked = false;
    };

    Impl(std::vector<ScreenBinding> bindings, gl::MaterialRegistry& registry, std::vector<ScreenGame> games,
         ScreenServiceOptions serviceOptions)
        : materials(registry), options(std::move(serviceOptions)),
          scheduler(ScreenSchedulerSettings{decoderBudget(options), options.video.proximityMeters,
                                            !options.lockstep || options.watchLateFrames}),
          slots(scheduler.slots().size()),
          pool(core::WorkerPoolOptions{
              .threads = 1, .capacity = 16, .belowNormalPriority = true, .name = "rv-screen-stills"}),
          cardFonts(std::make_shared<CardFonts>()), verdicts(options.verdictsFile, options.lockstep, kStillWait) {
        // I5 step 7: the clips known to fail never reach a screen (a lockstep run reads the verdicts first).
        if (options.lockstep) {
            verdicts.waitLoaded(kStillWait);
        }
        rawGames = games;
        games = verdicts.screened(std::move(games));
        for (ScreenBinding& binding : bindings) {
            ScreenPlaySettings settings = screenPlaySettings(options, binding);
            const double volume = options.video.screen(binding.kind, binding.roomId, binding.objectId).volume;
            screens.push_back(std::make_unique<ScreenState>(std::move(binding), ScreenPolicy(games, settings)));
            screens.back()->volume = volume;
        }
        lastScreenId = screens.empty() ? std::string() : screens.front()->binding.objectId;
        if (options.audio != nullptr) {
            for (Slot& slot : slots) {
                slot.source = options.audio->mixer().addSource();
            }
        }
    }

    /// The screens bound, the hero first (none between unbind() and bind()), the policies of screens let go (reused by
    /// the next bind: the clips that failed stay excluded), and the last hero's id (the record's at the end).
    std::vector<std::unique_ptr<ScreenState>> screens;
    std::vector<ScreenPolicy> sparePolicies;
    /// I5 step 4: the arcade cabinets asking for their clips (setCabinets()), their room's pooled video materials, those
    /// whose screens show their clips (cabinetsShown()), the games whose clips failed on a cabinet in the run.
    std::vector<std::unique_ptr<ScreenState>> cabinets;
    CabinetPool cabinetPool;
    std::vector<CabinetShown> shownCabinets;
    std::set<std::string, std::less<>> failedCabinetGames;
    std::vector<VideoCabinetEventRecord> pendingCabinetEvents;
    /// This frame's screens and cabinets, in that order: the scheduler's requests, the sound's candidates.
    std::vector<ScreenState*> active;
    std::string lastScreenId;
    gl::MaterialRegistry& materials;
    ScreenServiceOptions options;
    ScreenScheduler scheduler;
    std::vector<Slot> slots;
    core::WorkerPool pool;
    std::shared_ptr<CardFonts> cardFonts;
    /// The clips' verdicts (I5 step 7), and the room's games as given (screened again once the verdicts are read).
    ScreenVerdicts verdicts;
    std::vector<ScreenGame> rawGames;
    /// Explicit playlist ids already logged as unknown or without a clip (once per run).
    std::set<std::string, std::less<>> loggedPlaylistIds;
    /// Positions of clips stopped while still asked for (out of view, needed, the budget): reopened there.
    std::map<std::string, double, std::less<>> resumeAt;
    /// The game each clip opened for (the drops' records).
    std::map<std::string, std::string, std::less<>> clipGames;
    ScreenPhase phase = ScreenPhase::Running;
    bool reopenPending = false;
    std::uint64_t frameIndex = 0;
    double now = 0.0;
    /// This frame's window is minimized (every screen out of view).
    bool minimized = false;
    bool budgetErrorLogged = false;

    // The sound: the screen the sound follows (an index into screens; none: silence), the one it should follow given
    // the screens' wishes (the clip a slot opening gives its sound to), the screen it followed last (its id and game,
    // for the changes' record), the next level sample's time; the report's part.
    std::optional<std::size_t> audibleScreen;
    std::string intendedAudibleClip;
    std::string lastAudibleScreen;
    std::string lastAudibleGame;
    bool audibleRecorded = false;
    double nextLevelAt = 0.0;
    AudioRecord audioCounters;
    /// Frames the rings of earlier clips could not keep.
    std::uint64_t pastOverflowFrames = 0;

    // The run report's counters, and the phase and decoder state when the run ended (shutdown()).
    VideoRecord counters;
    std::optional<std::string> endPhase;
    std::optional<std::string> endDecoderState;

    [[nodiscard]] ScreenState* hero() const noexcept { return screens.empty() ? nullptr : screens.front().get(); }
    [[nodiscard]] bool isHero(const ScreenState& screen) const noexcept { return hero() == &screen; }
    /// "Hero CRT" or "Screen '<id>'", for the log.
    [[nodiscard]] std::string nameOf(const ScreenState& screen) const {
        return isHero(screen)    ? std::string("Hero CRT")
               : screen.cabinet ? fmt::format("Cabinet '{}'", screen.binding.objectId)
                                : fmt::format("Screen '{}'", screen.binding.objectId);
    }

    /// The screens bound, then the cabinets (I5 step 4).
    [[nodiscard]] std::vector<ScreenState*> allScreens() const {
        std::vector<ScreenState*> all;
        all.reserve(screens.size() + cabinets.size());
        for (const std::unique_ptr<ScreenState>& screen : screens) {
            all.push_back(screen.get());
        }
        for (const std::unique_ptr<ScreenState>& cabinet : cabinets) {
            all.push_back(cabinet.get());
        }
        return all;
    }

    /// The slot playing the hero's clip now, else none.
    [[nodiscard]] const Slot* heroPlayingSlot() const noexcept {
        const ScreenState* screen = hero();
        if (screen == nullptr || !screen->assignment.playing || !screen->assignment.slot) {
            return nullptr;
        }
        return &slots[*screen->assignment.slot];
    }
    /// The slot of the hero's last clip (its figures for the report): the one it played on, else the one assigned.
    [[nodiscard]] const Slot* heroClipSlot() const noexcept {
        const ScreenState* screen = hero();
        if (screen == nullptr) {
            return nullptr;
        }
        if (screen->lastSlot) {
            return &slots[*screen->lastSlot];
        }
        return screen->assignment.slot ? &slots[*screen->assignment.slot] : nullptr;
    }

    [[nodiscard]] std::string decoderStateName() const {
        const Slot* slot = heroPlayingSlot();
        return slot != nullptr && slot->playback && !slot->draining
                   ? std::string(media::toString(slot->playback->decoder().state()))
                   : std::string("none");
    }

    // --- decoders ------------------------------------------------------------------------------------------------

    void releaseDecoder(Slot& slot) {
        if (slot.playback) {
            slot.playback.reset();
            g_liveDecoders.fetch_sub(1, std::memory_order_relaxed);
        }
        slot.draining = false;
        slot.paused = false;
        slot.pending = nullptr;
        slot.pendingSince.reset();
    }

    /// Forgets the stopped decoders whose threads ended.
    void reapDrained() {
        for (Slot& slot : slots) {
            if (slot.draining && slot.playback && !slot.playback->decoder().threadRunning()) {
                releaseDecoder(slot);
            }
        }
    }

    [[nodiscard]] std::uint64_t decoderThreads() const {
        return static_cast<std::uint64_t>(std::count_if(slots.begin(), slots.end(), [](const Slot& slot) {
            return slot.playback && slot.playback->decoder().threadRunning();
        }));
    }

    /// Seconds into the clip of `slot` shown (timestamps keep growing across loops: taken modulo its duration).
    [[nodiscard]] static std::optional<double> clipPosition(const Slot& slot) {
        if (!slot.frameShown || !slot.lastPts) {
            return std::nullopt;
        }
        return slot.duration > 0.0 ? std::fmod(*slot.lastPts, slot.duration) : *slot.lastPts;
    }

    /// The screens asking for `clip` this frame, in their order.
    [[nodiscard]] std::vector<ScreenState*> requesters(std::string_view clip) const {
        std::vector<ScreenState*> found;
        for (ScreenState* screen : allScreens()) {
            if (!clip.empty() && screen->clip == clip) {
                found.push_back(screen);
            }
        }
        return found;
    }

    void addFallback(ScreenState& screen, ScreenPicture picture, std::string reason) {
        spdlog::warn("{}: the {} of game {} cannot be shown ({}); the next picture of the chain is shown",
                     nameOf(screen), toString(picture), screen.policy.targetId(), reason);
        if (counters.fallbacks.size() < kMaxTimelineEntries) {
            counters.fallbacks.push_back(
                VideoFallbackRecord{screen.policy.targetId(), std::string(toString(picture)), std::move(reason)});
        }
    }

    void recordEvent(std::size_t slot, SlotAction action, std::string gameId, std::string reason,
                     std::vector<std::string> shownOn, std::optional<double> fromSeconds = std::nullopt) {
        if (counters.slotEvents.size() < kMaxTimelineEntries) {
            counters.slotEvents.push_back(VideoSlotEventRecord{frameIndex, now, slot, std::string(toString(action)),
                                                               std::move(gameId), std::move(reason),
                                                               std::move(shownOn), fromSeconds});
        } else {
            ++counters.slotEventsDropped;
        }
    }

    [[nodiscard]] std::vector<std::string> screenIds(const std::vector<ScreenState*>& on) const {
        std::vector<std::string> ids;
        for (const ScreenState* screen : on) {
            ids.push_back(screen->binding.objectId);
        }
        return ids;
    }

    /// Stops the decoder of `index` (its thread ends on its own: the slot drains until the scheduler sees it ended).
    void stopSlot(std::size_t index, std::string_view reason) {
        Slot& slot = slots[index];
        if (!slot.playback || slot.draining) {
            return;
        }
        // A clip stopped while still asked for opens again where it was.
        if (reason == "out-of-view" || reason == "minimized" || reason == "needed" || reason == "budget") {
            if (const std::optional<double> position = clipPosition(slot)) {
                resumeAt[slot.clip] = *position;
            }
        }
        // I5 step 4 (ADR-0016 §6): a slot taken from a clip still asked for leaves its screens their frame, paused,
        // until the clip has a slot again (with one decoder, the hero cabinet's while a focused cabinet plays).
        if (reason == "needed" || reason == "budget") {
            keepFrames(index);
        }
        slot.playback->decoder().stop();
        if (slot.source) {
            // Its sound fades out (the pause's fade) with what its ring still holds.
            options.audio->mixer().setControl(*slot.source, audio::SourceControl{0.0, 0.0, false});
        }
        slot.draining = true;
        slot.paused = false;
        slot.pending = nullptr;
        slot.pendingSince.reset();
        slot.frameShown = false;
        recordEvent(index, SlotAction::Stop, slot.gameId, std::string(reason), screenIds(requesters(slot.clip)));
        spdlog::info("Video slot {}: the clip of game {} stops ({})", index, slot.gameId, reason);
    }

    /// The bound screens showing the picture of slot `index` keep a copy of it (heldFrame), the slot's texture going on
    /// with another clip.
    void keepFrames(std::size_t index) {
        const Slot& slot = slots[index];
        if (!slot.texture.valid() || !slot.frameShown) {
            return;
        }
        for (const std::unique_ptr<ScreenState>& screen : screens) {
            if (screen->overrideSlot != index || screen->appliedOverride != slot.texture.textureId() ||
                screen->shownPicture != ScreenPicture::Video) {
                continue;
            }
            gl::TextureDesc desc;
            desc.colorSpace = gl::ColorSpace::Srgb;
            desc.mipmaps = false;
            desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
            desc.memoryKind = gl::GpuMemoryKind::HeroTextures;
            auto copy = gl::Texture2D::copyOf(slot.texture.textureId(), slot.texture.size(), desc, "Screen.HeldFrame");
            if (!copy) {
                spdlog::warn("{}: its frame cannot be kept ({}); its still shows until its clip plays again",
                             nameOf(*screen), copy.error().toString());
                continue;
            }
            screen->heldFrame = std::move(*copy);
            screen->heldFrameGame = screen->policy.targetId();
            screen->heldFrameRect = screen->rect;
            spdlog::info("{}: its slot was taken; it keeps the frame of game {} until its clip plays again",
                         nameOf(*screen), screen->heldFrameGame);
        }
    }

    void openSlot(std::size_t index, const std::string& clip) {
        Slot& slot = slots[index];
        const std::vector<ScreenState*> on = requesters(clip);
        if (slot.playback) {
            // The scheduler opens only a slot whose thread ended (feedback): forget the stopped decoder.
            assert(!slot.playback->decoder().threadRunning() && "a slot reopened before its thread ended");
            releaseDecoder(slot);
        }
        if (on.empty()) {
            scheduler.openFailed(index);
            return;
        }
        if (g_liveDecoders.load(std::memory_order_relaxed) >= scheduler.settings().maxDecoders) {
            // NFR-07: never more decoders alive than the budget's slots (another service holds them: none does).
            assert(false && "never more video decoders than the budget");
            spdlog::error("Video slot {}: a decoder over the budget of {} was refused", index,
                          scheduler.settings().maxDecoders);
            scheduler.openFailed(index);
            return;
        }
        ScreenState& first = *on.front();
        media::DecoderSettings settings;
        settings.maxHeight = options.video.maxRows;
        settings.loop = true;
        settings.startPaused = options.freezePtsSeconds.has_value();
        settings.name = fmt::format("screen-{}", index);
        std::shared_ptr<audio::AudioRing> ring;
        if (slot.source) {
            // The clip's sound into a fresh ring; decoded from the start when its screen is the one to be heard.
            ring = std::make_shared<audio::AudioRing>(options.audio->mixer().settings().sampleRate);
            settings.audioSink = std::make_shared<RingSink>(ring);
            settings.audioSampleRate = options.audio->mixer().settings().sampleRate;
            settings.audioEnabled = clip == intendedAudibleClip && !options.freezePtsSeconds;
        }
        auto decoder = media::VideoDecoder::start(first.policy.targetMedia().video, std::move(settings));
        if (!decoder) {
            for (ScreenState* screen : on) {
                addFallback(*screen, ScreenPicture::Video, std::string(toString(decoder.error().code)));
                screen->policy.videoFailed();
            }
            scheduler.openFailed(index);
            return;
        }
        g_liveDecoders.fetch_add(1, std::memory_order_relaxed);
        ++counters.decodersStarted;
        slot.playback = std::make_unique<media::VideoPlayback>(std::move(*decoder));
        if (slot.source) {
            if (slot.ring) {
                pastOverflowFrames += slot.ring->stats().overflowFrames;
            }
            slot.ring = ring;
            options.audio->mixer().setRing(*slot.source, std::move(ring));
            slot.audioRecorded = false;
        }
        slot.draining = false;
        slot.paused = false;
        slot.clip = clip;
        slot.gameId = first.policy.targetId();
        clipGames[clip] = slot.gameId;
        slot.textureFailed = false;
        slot.failureHandled = false;
        slot.started = Clock::now();
        slot.frameShown = false;
        slot.duration = 0.0;
        slot.firstFrameMs.reset();
        slot.frames = 0;
        slot.firstPts.reset();
        slot.lastPts.reset();
        slot.firstUploadAt.reset();
        slot.frozenShown = false;
        slot.firstFrameWaited = false;
        slot.pending = nullptr;
        slot.pendingSince.reset();
        slot.media = first.policy.targetMedia();
        slot.passChecked = false;
        // Where it opens: a launch's position (its screens' policies), else one kept when it stopped.
        std::optional<double> resume;
        for (ScreenState* screen : on) {
            if (const std::optional<double> position = screen->policy.takeResumePosition(); position && !resume) {
                resume = position;
            }
        }
        if (!resume) {
            if (const auto kept = resumeAt.find(clip); kept != resumeAt.end()) {
                resume = kept->second;
                resumeAt.erase(kept);
            }
        }
        slot.fromStart = !resume && !options.freezePtsSeconds;
        const bool heroOn = std::find(on.begin(), on.end(), hero()) != on.end();
        if (reopenPending && heroOn && !counters.launches.empty()) {
            reopenPending = false;
            counters.launches.back().reopened = true;
            counters.launches.back().reopenedAtSeconds = resume;
        }
        if (options.freezePtsSeconds) {
            slot.playback->pause(now);
            slot.playback->seek(*options.freezePtsSeconds);
            slot.paused = true;
        } else if (resume) {
            slot.playback->seek(*resume);
        }
        recordEvent(index, SlotAction::Open, slot.gameId, "wanted", screenIds(on),
                    options.freezePtsSeconds ? options.freezePtsSeconds : resume);
        spdlog::info("Video slot {}: the video of game {} opens on {} screen(s){}", index, slot.gameId, on.size(),
                     options.freezePtsSeconds
                         ? fmt::format(", held at {:g} s (--video-freeze-pts)", *options.freezePtsSeconds)
                     : resume ? fmt::format(" at {:.3f} s, where it stopped", *resume)
                              : std::string());
        spdlog::debug("Video slot {}: video file {}", index, core::pathToUtf8Lossy(first.policy.targetMedia().video));
    }

    void applyCommands(const std::vector<SlotCommand>& commands) {
        for (const SlotCommand& command : commands) {
            Slot& slot = slots[command.slot];
            // While the window is minimized every screen is out of view: its pauses and stops are the window's.
            const std::string reason =
                minimized && command.reason == "out-of-view" ? std::string("minimized") : command.reason;
            switch (command.action) {
            case SlotAction::Stop:
                stopSlot(command.slot, reason);
                break;
            case SlotAction::Open:
                openSlot(command.slot, command.clip);
                break;
            case SlotAction::Pause:
                // Every screen showing it left the view: the frame kept. A frozen run's clips stay paused anyway.
                if (slot.playback && !slot.draining && !options.freezePtsSeconds && !slot.paused) {
                    slot.playback->pause(now);
                    slot.paused = true;
                    ++counters.pauses;
                }
                recordEvent(command.slot, SlotAction::Pause, slot.gameId, reason, screenIds(requesters(slot.clip)));
                break;
            case SlotAction::Resume:
                if (slot.playback && !slot.draining && !options.freezePtsSeconds && slot.paused) {
                    slot.playback->resume(now);
                    slot.paused = false;
                }
                recordEvent(command.slot, SlotAction::Resume, slot.gameId, command.reason,
                            screenIds(requesters(slot.clip)));
                break;
            }
        }
    }

    /// The scheduler's frame: the slots' feedback, the screens' requests, its commands carried out.
    void schedule() {
        reapDrained();
        std::vector<SlotFeedback> feedback(slots.size());
        for (std::size_t index = 0; index < slots.size(); ++index) {
            const Slot& slot = slots[index];
            feedback[index].threadRunning = slot.playback && slot.playback->decoder().threadRunning();
            if (slot.playback && !slot.draining) {
                const media::PresentationStats& stats = slot.playback->clock().stats();
                feedback[index].presented = stats.presented;
                feedback[index].late = stats.late;
                feedback[index].dropped = stats.dropped;
            }
        }
        std::vector<ScreenRequest> requests;
        requests.reserve(active.size());
        for (const ScreenState* screen : active) {
            // The hero's own slot is the hero's, whichever room's: a room changed reopens on it (one thread).
            requests.push_back(ScreenRequest{screen->binding.objectId, screen->screenClass, screen->clip,
                                             screen->distance, screen->visible,
                                             isHero(*screen) ? std::string(kHeroScreenId) : std::string()});
        }
        const std::size_t drops = scheduler.drops().size();
        const ScheduleResult result = scheduler.update(now, requests, feedback);
        for (std::size_t index = drops; index < scheduler.drops().size(); ++index) {
            const BudgetDrop& drop = scheduler.drops()[index];
            spdlog::warn("Video: {:.1f}% of slot {}'s frames came late over {:g} s: one decoder for {:g} s",
                         100.0 * drop.ratio, drop.slot, kLateWindowSeconds, kLateDropSeconds);
        }
        applyCommands(result.commands);
        for (std::size_t index = 0; index < active.size(); ++index) {
            active[index]->assignment = result.screens[index];
        }
        // NFR-07 (ADR-0016 §2): never more decoders open than the budget, nor more alive than its slots.
        const std::size_t open = scheduler.openSlots();
        counters.maxOpenDecoders = std::max<std::uint64_t>(counters.maxOpenDecoders, open);
        const int live = g_liveDecoders.load(std::memory_order_relaxed);
        if ((open > static_cast<std::size_t>(scheduler.budget(now)) || live > scheduler.settings().maxDecoders) &&
            !budgetErrorLogged) {
            assert(false && "more video decoders than the budget");
            spdlog::error("Video: {} decoder(s) open and {} alive over the budget of {}", open, live,
                          scheduler.budget(now));
            budgetErrorLogged = true;
        }
    }

    // --- frames --------------------------------------------------------------------------------------------------

    /// Uploads the pending frame of `index`; false when it stays pending (the ring busy) or failed.
    bool upload(std::size_t index) {
        Slot& slot = slots[index];
        const media::VideoFrame& frame = *slot.pending;
        const glm::ivec2 size(frame.width(), frame.height());
        // The texture and its ring are made once, outside the section: it times the uploads.
        if (!slot.texture.valid()) {
            auto created = gl::VideoTexture::create(size, fmt::format("Screen.Video{}", index));
            if (!created) {
                spdlog::error("Video slot {}: no video texture: {}", index, created.error().toString());
                slot.textureFailed = true;
                return false;
            }
            slot.texture = std::move(*created);
        }
        Expected<gl::VideoUpload> uploaded = gl::VideoUpload::RingBusy;
        {
            const core::CpuSectionScope timing(core::cpu_section::kVideoUpload);
            uploaded = slot.texture.upload(frame.bytes(), size, frame.strideBytes());
        }
        if (!uploaded) {
            spdlog::error("Video slot {}: a video frame was not uploaded: {}", index, uploaded.error().toString());
            slot.playback->releaseFrame();
            slot.pending = nullptr;
            slot.pendingSince.reset();
            return false;
        }
        if (*uploaded == gl::VideoUpload::RingBusy) {
            ++counters.ringBusy; // kept pending: retried at the next frame
            return false;
        }
        slot.playback->releaseFrame();
        slot.pending = nullptr;
        slot.pendingSince.reset();
        ++counters.framesUploaded;
        ++slot.frames;
        if (const std::optional<media::FrameTiming>& timing = slot.playback->clock().current()) {
            if (!slot.firstPts) {
                slot.firstPts = timing->ptsSeconds;
            }
            slot.lastPts = timing->ptsSeconds;
            checkPass(slot);
            if (options.recordPresentedFrames && audibleSlot() == index &&
                audioCounters.presented.size() < kMaxAudioTimelineEntries) {
                audioCounters.presented.push_back(AudioPresentedRecord{now, timing->ptsSeconds});
            }
        }
        if (!slot.firstUploadAt) {
            slot.firstUploadAt = now;
        }
        slot.lastUploadAt = now;
        if (options.freezePtsSeconds) {
            slot.frozenShown = true;
        }
        if (!slot.frameShown) {
            slot.frameShown = true;
            slot.firstFrameMs = millisecondsSince(slot.started);
            if (const std::optional<media::StreamInfo> info = slot.playback->decoder().streamInfo()) {
                slot.duration = info->durationSeconds;
            }
            spdlog::info("Video slot {}: the first frame of game {}'s video after {:.1f} ms", index, slot.gameId,
                         *slot.firstFrameMs);
        }
        return true;
    }

    /// A lockstep run (not frozen: that run waits for its own frame): the first frame of a clip a slot opened for a
    /// screen in view is waited for, at most kFirstFrameWait, once per opening, so that it shows on the frame the slot
    /// opened whatever the decoder thread's pace (the waits recorded: VideoRecord::firstFrameWaits).
    void awaitFirstFrame(Slot& slot) {
        if (!options.lockstep || options.freezePtsSeconds || slot.firstFrameWaited || slot.frameShown ||
            slot.pending != nullptr || slot.paused || slot.textureFailed || slot.playback->failed()) {
            return;
        }
        const std::vector<ScreenState*> on = requesters(slot.clip);
        if (std::none_of(on.begin(), on.end(), [](const ScreenState* screen) { return screen->visible; })) {
            return;
        }
        slot.firstFrameWaited = true;
        const Clock::time_point start = Clock::now();
        const Clock::time_point deadline = start + kFirstFrameWait;
        while (slot.pending == nullptr && !slot.playback->failed() && Clock::now() < deadline) {
            std::this_thread::sleep_for(kPollInterval);
            slot.pending = slot.playback->update(now).frame;
        }
        const double waited = millisecondsSince(start);
        ++counters.firstFrameWaits;
        counters.firstFrameWaitMaxMs = std::max(counters.firstFrameWaitMaxMs, waited);
        if (slot.pending != nullptr) {
            slot.pendingSince = frameIndex;
        } else if (!slot.playback->failed()) {
            ++counters.firstFrameWaitTimeouts;
            spdlog::warn("Video: the first frame of game {}'s clip did not arrive within {} s (lockstep)", slot.gameId,
                         kFirstFrameWait.count());
        }
    }

    /// Takes each open slot's frame of this time, then uploads them within kMaxVideoUploadBytesPerFrame, the longest
    /// pending first (ADR-0016 §2); a frame left over waits for the next render frame (or is superseded).
    void pumpSlots() {
        for (Slot& slot : slots) {
            if (!slot.playback || slot.draining) {
                continue;
            }
            const media::PlaybackUpdate update = slot.playback->update(now);
            slot.pending = update.frame;
            if (slot.pending == nullptr) {
                slot.pendingSince.reset();
            } else if (!slot.pendingSince) {
                slot.pendingSince = frameIndex;
            }
            if (options.freezePtsSeconds && !slot.frozenShown && slot.pending == nullptr && !slot.textureFailed) {
                // The frame at the frozen time is what the screenshots show: it is waited for (the decoder delivers
                // it a few milliseconds after the seek).
                const Clock::time_point deadline = Clock::now() + kFreezeWait;
                while (slot.pending == nullptr && !slot.playback->failed() && Clock::now() < deadline) {
                    std::this_thread::sleep_for(kPollInterval);
                    slot.pending = slot.playback->update(now).frame;
                }
                if (slot.pending != nullptr) {
                    slot.pendingSince = frameIndex;
                } else if (!slot.playback->failed()) {
                    spdlog::warn("Video: the frame of game {} at {:g} s did not arrive within {} s", slot.gameId,
                                 *options.freezePtsSeconds, kFreezeWait.count());
                }
            }
        }
        std::vector<std::size_t> order;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (slots[index].pending != nullptr) {
                order.push_back(index);
            }
            awaitFirstFrame(slot);
        }
        std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            return slots[a].pendingSince.value_or(frameIndex) < slots[b].pendingSince.value_or(frameIndex);
        });
        std::size_t bytes = 0;
        for (const std::size_t index : order) {
            Slot& slot = slots[index];
            if (slot.textureFailed) {
                slot.playback->releaseFrame();
                slot.pending = nullptr;
                continue;
            }
            const std::size_t frameBytes = static_cast<std::size_t>(slot.pending->width()) *
                                           static_cast<std::size_t>(slot.pending->height()) * 4u;
            if (bytes > 0 && bytes + frameBytes > kMaxVideoUploadBytesPerFrame) {
                ++counters.deferredUploads; // pending until the next frame
                continue;
            }
            if (upload(index)) {
                bytes += frameBytes;
            }
        }
        counters.maxUploadBytes = std::max<std::uint64_t>(counters.maxUploadBytes, bytes);
        if (bytes > kMaxVideoUploadBytesPerFrame) {
            ++counters.oversizeUploads; // one frame alone larger than the budget
        }
    }

    /// A slot whose decoder failed (or whose texture could not be made): its screens go to their stills, and the clip
    /// leaves their playlists; the scheduler stops it at the next frame (no screen asks for it any more).
    void checkFailures() {
        for (Slot& slot : slots) {
            if (!slot.playback || slot.draining || slot.failureHandled ||
                !(slot.playback->failed() || slot.textureFailed)) {
                continue;
            }
            slot.failureHandled = true;
            const std::string reason = slot.textureFailed ? std::string(toString(ErrorCode::GraphicsError))
                                                          : failureName(slot.playback->decoder().failure());
            for (ScreenState* screen : requesters(slot.clip)) {
                addFallback(*screen, ScreenPicture::Video, reason);
                screen->policy.videoFailed();
                if (screen->cabinet) {
                    failedCabinetGames.insert(screen->policy.targetId()); // its cover for the rest of the run
                }
            }
            if (!slot.textureFailed) {
                // I5 step 7: remembered, so that it is never opened again while its file is unchanged.
                const media::VideoDecoder& decoder = slot.playback->decoder();
                verdicts.clipFailed(slot.media, slot.gameId, decoder.failure(), decoder.streamInfo(), decoder.stats());
            }
        }
    }

    /// A clip opened at its start whose first pass ended (a frame of its second shows): playable when the decoder
    /// counted no error and no damaged frame (I5 step 7). Looked at once per opening.
    void checkPass(Slot& slot) {
        if (slot.passChecked || !slot.fromStart || slot.duration <= 0.0 || !slot.lastPts ||
            *slot.lastPts < slot.duration) {
            return;
        }
        slot.passChecked = true;
        const media::VideoDecoder& decoder = slot.playback->decoder();
        const media::DecoderStats stats = decoder.stats();
        if (stats.loops >= 1 && stats.decodeErrors == 0 && stats.damagedFrames == 0) {
            verdicts.clipPlayed(slot.media, slot.gameId, decoder.streamInfo());
        }
    }

    /// Tells the screen's policy whether its clip shows (its slot's first frame came) or no longer does, and whether
    /// it has a slot at all.
    void followSlot(ScreenState& screen) {
        const ScreenAssignment& assignment = screen.assignment;
        const Slot* slot = assignment.playing && assignment.slot ? &slots[*assignment.slot] : nullptr;
        const bool playing = slot != nullptr && slot->playback && !slot->draining && slot->clip == screen.clip;
        if (playing && slot->frameShown && !screen.videoTold) {
            screen.policy.videoShowing();
            screen.policy.clipDuration(slot->duration);
            screen.videoTold = true;
            screen.lastSlot = assignment.slot;
        } else if (!playing && screen.videoTold) {
            screen.policy.videoStopped(); // its still again until the clip reopens
            screen.videoTold = false;
        }
        screen.policy.setNoSlot(screen.policy.wantsDecoder() && !assignment.playing && !assignment.waiting);
    }

    // --- stills --------------------------------------------------------------------------------------------------

    void requestStill(ScreenState& screen) {
        if (screen.stillLoading) {
            return;
        }
        const std::optional<ScreenPicture> wanted = screen.policy.wantedStill();
        if (!wanted) {
            return;
        }
        const std::filesystem::path* file = mediaPath(screen.policy.targetMedia(), *wanted);
        const std::filesystem::path path = file != nullptr ? *file : std::filesystem::path();
        const bool card = *wanted == ScreenPicture::TitleCard;
        const std::uint64_t token = ++screen.stillToken;
        const std::string context = fmt::format("{} of game {}", toString(*wanted), screen.policy.targetId());
        auto job = [slot = screen.stillSlot, token, path, context, card, fonts = cardFonts,
                    assetRoot = options.assetRoot, title = screen.policy.targetCard()](const core::JobContext& job) {
            StillResult result{token, assets::ImageData{}};
            if (job.cancelled()) {
                result.picture = makeError(ErrorCode::Cancelled, "cancelled", context);
            } else if (card) {
                result.picture = drawCard(*fonts, assetRoot, title); // the chain's end (ADR-0016 §8)
            } else {
                result.picture = loadStill(path, context);
            }
            const std::scoped_lock lock(slot->mutex);
            slot->result = std::move(result);
        };
        core::JobOptions jobOptions;
        jobOptions.generation = &screen.stillJobs;
        if (auto submitted = pool.submit(std::move(job), jobOptions); !submitted) {
            addFallback(screen, *wanted, std::string(toString(submitted.error().code)));
            screen.policy.stillFailed();
            return;
        }
        screen.stillLoading = true;
        screen.loadingPicture = wanted;
    }

    /// The result of the load in flight, if it arrived (a stale one is dropped).
    static std::optional<StillResult> takeStill(ScreenState& screen) {
        const std::scoped_lock lock(screen.stillSlot->mutex);
        if (!screen.stillSlot->result) {
            return std::nullopt;
        }
        std::optional<StillResult> result = std::exchange(screen.stillSlot->result, std::nullopt);
        if (result->token != screen.stillToken) {
            return std::nullopt;
        }
        return result;
    }

    void updateStills(ScreenState& screen) {
        // A failed still asks for the next one at once: a chain of missing files falls through within the frame.
        for (int guard = 0; guard < 4; ++guard) {
            requestStill(screen);
            if (!screen.stillLoading) {
                return;
            }
            std::optional<StillResult> result = takeStill(screen);
            if (!result && options.lockstep) {
                const Clock::time_point deadline = Clock::now() + kStillWait;
                while (!result && Clock::now() < deadline) {
                    std::this_thread::sleep_for(kPollInterval);
                    result = takeStill(screen);
                }
            }
            if (!result) {
                return;
            }
            screen.stillLoading = false;
            const ScreenPicture picture = screen.loadingPicture.value_or(ScreenPicture::Cover);
            if (result->picture) {
                gl::TextureDesc desc;
                desc.colorSpace = gl::ColorSpace::Srgb;
                desc.mipmaps = true;
                desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
                desc.memoryKind = gl::GpuMemoryKind::HeroTextures;
                auto texture = gl::Texture2D::create(*result->picture, desc, "Screen.Still");
                if (texture) {
                    screen.still = std::move(*texture);
                    screen.policy.stillReady();
                    return;
                }
                addFallback(screen, picture, std::string(toString(texture.error().code)));
            } else {
                spdlog::debug("{}: {}", nameOf(screen), result->picture.error().toString());
                addFallback(screen, picture, std::string(toString(result->picture.error().code)));
            }
            screen.policy.stillFailed();
        }
    }

    // --- the screens ---------------------------------------------------------------------------------------------

    /// A new target: its first still loads; the picture shown stays up until the new one's is ready (its still, or
    /// its first frame in its slot's texture). Its previous clip stops through the scheduler (no screen asks for it).
    void followTarget(ScreenState& screen) {
        ScreenPolicy& policy = screen.policy;
        if (screen.followed && policy.targetGeneration() == screen.followedGeneration) {
            return;
        }
        screen.followed = true;
        screen.followedGeneration = policy.targetGeneration();
        if (isHero(screen) && !policy.targetId().empty()) {
            ++counters.targets;
            if (counters.played.size() < kMaxTimelineEntries) {
                counters.played.push_back(
                    VideoPlayedRecord{now, policy.targetId(), std::string(toString(policy.origin()))});
            }
        }
        pool.cancel(screen.stillJobs);
        ++screen.stillToken; // a result of the previous target is dropped
        screen.stillLoading = false;
        screen.loadingPicture.reset();
        if (screen.still.valid() && screen.appliedOverride == screen.still.id()) {
            screen.heldStill = std::move(screen.still); // on the screen until the new target's picture is ready
        }
        screen.still = gl::Texture2D();
        screen.videoTold = false;
        if (policy.targetId().empty()) {
            spdlog::info("{}: no game to show, the idle pattern", nameOf(screen));
        } else {
            spdlog::info("{}: game {} ({})", nameOf(screen), policy.targetId(),
                         policy.origin() == ScreenOrigin::Selection ? "selected"
                         : policy.origin() == ScreenOrigin::Playlist
                             ? fmt::format("the {} playlist's", toString(policy.playlist().mode()))
                             : std::string("the start choice"));
            if (!policy.targetMedia().video.empty() && !isPlayableVideoFile(policy.targetMedia().video)) {
                addFallback(screen, ScreenPicture::Video, "not-playable");
            }
        }
    }

    void applyPicture(ScreenState& screen) {
        const ScreenPolicy& policy = screen.policy;
        ScreenPicture picture = policy.picture();
        const bool heroScreen = isHero(screen);
        // I5 step 4 (ADR-0016 §6): the frame kept when its slot was taken shows while its clip waits for a slot again
        // (its game still the target, its clip still wanted), in place of its still.
        const bool keepFrame = screen.heldFrame.valid() && picture != ScreenPicture::Video &&
                               screen.heldFrameGame == policy.targetId() && policy.wantsDecoder();
        screen.showingHeldFrame = keepFrame;
        if (keepFrame) {
            screen.held.reset();
            ++counters.heldFrameShows;
            applyHeldFrame(screen);
            return;
        }
        if (picture == ScreenPicture::IdlePattern && !policy.targetId().empty()) {
            // Between two games the previous picture stays while the new one can still come, at most
            // kMaxHeldPictureSeconds; then (or when nothing can come) the screen's own pattern.
            const bool arriving = policy.wantsDecoder() || policy.wantedStill().has_value();
            const double wallSeconds = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            if (screen.appliedOverride != 0 && heldPictureThere(screen) &&
                screen.held.hold(arriving, now, wallSeconds)) {
                if (heroScreen) {
                    ++counters.heldFrames;
                }
                return;
            }
            if (heroScreen) {
                ++counters.idleFramesWithGame; // nothing shown yet since the screen was bound, or the hold ran out
            }
        } else {
            screen.held.reset();
        }
        GLuint texture = 0;
        glm::vec4 rect = kWholeScreen;
        if (picture == ScreenPicture::Video && screen.lastSlot) {
            const Slot& slot = slots[*screen.lastSlot];
            texture = slot.texture.textureId();
            // ADR-0016 §7: the clip keeps its aspect on the screen's (a 4:3 clip on a 4:3 tube: the whole screen).
            rect = pictureRect(glm::vec2(slot.texture.size()), screen.binding.screenSize);
        } else if (picture != ScreenPicture::IdlePattern && picture != ScreenPicture::Video) {
            texture = screen.still.id();
        }
        setRect(screen, rect);
        if (picture != screen.shownPicture || policy.targetId() != screen.shownGame) {
            if (heroScreen && counters.shown.size() < kMaxTimelineEntries) {
                counters.shown.push_back(VideoShownRecord{now, policy.targetId(), std::string(toString(picture))});
            }
            screen.shownGame = policy.targetId();
        }
        if (texture == screen.appliedOverride && picture == screen.shownPicture) {
            releaseHeldStill(screen);
            releaseHeldFrame(screen);
            return;
        }
        if (auto set =
                materials.setTextureOverride(screen.binding.material, gl::MaterialTextureSlot::BaseColor, texture);
            !set) {
            spdlog::error("{}: the screen's picture cannot be set: {}", nameOf(screen), set.error().toString());
            return;
        }
        if (picture != screen.shownPicture) {
            spdlog::info("{} shows {}{}", nameOf(screen), toString(picture),
                         policy.targetId().empty() ? std::string() : " of game " + policy.targetId());
        }
        screen.appliedOverride = texture;
        screen.shownPicture = picture;
        screen.overrideSlot.reset();
        screen.overrideClip.clear();
        if (picture == ScreenPicture::Video && screen.lastSlot) {
            screen.overrideSlot = screen.lastSlot;
            screen.overrideClip = slots[*screen.lastSlot].clip;
        }
        releaseHeldStill(screen);
        releaseHeldFrame(screen);
    }

    /// The screen's material draws its picture within `rect` (ADR-0016 §7; the whole screen clears the flag).
    void setRect(ScreenState& screen, const glm::vec4& rect) {
        if (screen.cabinet || rect == screen.rect) {
            return;
        }
        const std::optional<glm::vec4> set = isWholeScreen(rect) ? std::nullopt : std::optional<glm::vec4>(rect);
        if (auto done = materials.setPictureRect(screen.binding.material, set); !done) {
            spdlog::error("{}: the picture's rectangle cannot be set: {}", nameOf(screen), done.error().toString());
            return;
        }
        if (!isWholeScreen(rect)) {
            spdlog::info("{}: the picture fills ({:.3f}, {:.3f}) {:.3f} x {:.3f} of the screen", nameOf(screen), rect.x,
                         rect.y, rect.z, rect.w);
        }
        screen.rect = rect;
    }

    /// Shows the frame the screen kept (keepFrames()) with its rectangle.
    void applyHeldFrame(ScreenState& screen) {
        setRect(screen, screen.heldFrameRect);
        const GLuint texture = screen.heldFrame.id();
        if (texture == screen.appliedOverride) {
            return;
        }
        if (auto set =
                materials.setTextureOverride(screen.binding.material, gl::MaterialTextureSlot::BaseColor, texture);
            !set) {
            spdlog::error("{}: the screen's picture cannot be set: {}", nameOf(screen), set.error().toString());
            return;
        }
        screen.appliedOverride = texture;
        screen.shownPicture = ScreenPicture::Video;
        screen.overrideSlot.reset();
        screen.overrideClip.clear();
        releaseHeldStill(screen);
    }

    /// The kept frame goes once the screen no longer shows it.
    static void releaseHeldFrame(ScreenState& screen) {
        if (screen.heldFrame.valid() && screen.heldFrame.id() != screen.appliedOverride) {
            screen.heldFrame = gl::Texture2D();
            screen.heldFrameGame.clear();
        }
    }

    /// The picture the screen holds is still what it showed: a still is its own (heldStill); a video is its slot's
    /// texture while the slot keeps that texture and does not play another screen's clip (it would show that game; a
    /// reallocated texture is gone). The slot reopened for one of this screen's own clips keeps the hold.
    [[nodiscard]] bool heldPictureThere(const ScreenState& screen) const {
        if (!screen.overrideSlot) {
            return true;
        }
        const Slot& slot = slots[*screen.overrideSlot];
        if (slot.texture.textureId() != screen.appliedOverride) {
            return false;
        }
        if (slot.clip == screen.overrideClip || slot.clip == screen.clip) {
            return true;
        }
        const std::vector<ScreenState*> on = requesters(slot.clip);
        return on.empty();
    }

    /// The held still goes once the screen no longer shows it.
    static void releaseHeldStill(ScreenState& screen) {
        if (screen.heldStill.valid() && screen.heldStill.id() != screen.appliedOverride) {
            screen.heldStill = gl::Texture2D();
        }
    }

    /// Logs the playlist the screen plays, and the explicit ids it skips (each once in the run).
    void logPlaylist(const ScreenState& screen) {
        const ScreenPolicy& policy = screen.policy;
        const Playlist& playlist = policy.playlist();
        spdlog::info("{}: the {} playlist of {} ({} game(s) with a clip), {:g} s a game{}", nameOf(screen),
                     toString(playlist.mode()), policy.settings().playlist.roomId, playlist.size(),
                     policy.settings().previewSeconds, policy.settings().autoplay ? "" : " (autoplay off)");
        for (const std::string& id : playlist.unknownIds()) {
            if (loggedPlaylistIds.insert(id).second) {
                spdlog::warn("{}: video.json's explicit playlist of {} names {}, which the room does not have: "
                             "skipped",
                             nameOf(screen), policy.settings().playlist.roomId, id);
            }
        }
        for (const std::string& id : playlist.unplayableIds()) {
            if (loggedPlaylistIds.insert(id).second) {
                spdlog::warn("{}: video.json's explicit playlist of {} names {}, which has no clip to play: skipped",
                             nameOf(screen), policy.settings().playlist.roomId, id);
            }
        }
    }

    [[nodiscard]] static ScreenClass classOf(const ScreenState& screen) noexcept {
        if (screen.policy.selected()) {
            return ScreenClass::Focus;
        }
        return screen.binding.kind == ScreenKind::Hero ? ScreenClass::Room : ScreenClass::Ambient;
    }

    void update(std::uint64_t frame, double nowSeconds, const scene::Camera& camera, const ScreenSelection& selection,
                bool minimizedWindow, bool eyeInRoom) {
        frameIndex = frame;
        now = nowSeconds;
        minimized = minimizedWindow;
        flushCabinetEvents();
        if (phase == ScreenPhase::Quit) {
            return;
        }
        if (screens.empty()) {
            // Unbound (a floor being committed again): the previous clips' threads end on their own meanwhile.
            reapDrained();
            counters.maxDecoderThreads = std::max(counters.maxDecoderThreads, decoderThreads());
            return;
        }
        if (verdicts.takeChanged()) {
            // The verdicts came after the games, or a clip left out turned out to have changed (I5 step 7): the clips
            // known to fail leave the games now, the others come back.
            const std::vector<ScreenGame> screenedGames = verdicts.screened(rawGames);
            for (const std::unique_ptr<ScreenState>& screen : screens) {
                screen->policy.setGames(screenedGames);
            }
            dropCabinets(); // asked for again next frame, with the games as screened now
        }
        const scene::Frustum frustum = camera.frustum();
        // The quick catalog's selection, else the world's (app::screenSelection's order).
        const std::optional<std::string_view> selected = selection.catalog ? selection.catalog : selection.world;
        for (const std::unique_ptr<ScreenState>& screen : screens) {
            // I5 step 4: the arcade's hero cabinet never follows the world's selection, the focused cabinet playing its
            // clip itself (ADR-0016 §6); it follows the quick catalog's.
            screen->policy.hover(screen->binding.followsWorldSelection ? selected : selection.catalog, now);
            screen->visible = !minimizedWindow && frustum.intersects(screen->binding.bounds);
            screen->policy.setVisible(screen->visible);
            screen->policy.tick(now);
            followTarget(*screen);
            screen->screenClass = classOf(*screen);
            screen->distance = glm::distance(camera.position(), screen->binding.bounds.center());
            const glm::vec3 center = screen->binding.bounds.center();
            const glm::vec3 forward = camera.forward();
            screen->azimuth = azimuthOf(camera.position().x, camera.position().z, forward.x, forward.z, center.x,
                                        center.z);
            screen->clip = screen->policy.wantsDecoder() ? clipKey(screen->policy.targetMedia().video) : std::string();
        }
        // I5 step 4: the cabinets asking for their clips, in view (one in the hand always) and at their distance.
        for (const std::unique_ptr<ScreenState>& cabinet : cabinets) {
            cabinet->policy.hover(std::nullopt, now);
            cabinet->visible = !minimizedWindow && (cabinet->inHand || frustum.intersects(cabinet->binding.bounds));
            cabinet->policy.setVisible(cabinet->visible);
            cabinet->policy.tick(now);
            const glm::vec3 center = cabinet->binding.bounds.center();
            cabinet->distance = cabinet->inHand ? kInHandMeters : glm::distance(camera.position(), center);
            const glm::vec3 forward = camera.forward();
            cabinet->azimuth = cabinet->inHand ? 0.0
                                               : azimuthOf(camera.position().x, camera.position().z, forward.x,
                                                           forward.z, center.x, center.z);
            cabinet->clip =
                cabinet->policy.wantsDecoder() ? clipKey(cabinet->policy.targetMedia().video) : std::string();
        }
        active = allScreens();
        // The screen heard last frame, among this frame's (cabinets come and go).
        audibleScreen.reset();
        for (std::size_t index = 0; index < active.size() && !lastAudibleScreen.empty(); ++index) {
            if (active[index]->binding.objectId == lastAudibleScreen) {
                audibleScreen = index;
            }
        }
        // The clip whose sound a slot opening this frame decodes from the start: the one the audible screen wants.
        intendedAudibleClip.clear();
        if (const std::optional<std::size_t> intended = chooseAudibleScreen(eyeInRoom, true)) {
            intendedAudibleClip = active[*intended]->clip;
        }
        schedule();
        pumpSlots();
        checkFailures();
        for (const std::unique_ptr<ScreenState>& screen : screens) {
            followSlot(*screen);
            updateStills(*screen);
            applyPicture(*screen);
        }
        applyCabinets();
        updateAudio(eyeInRoom);
        counters.maxDecoderThreads = std::max(counters.maxDecoderThreads, decoderThreads());
    }

    // --- the sound -----------------------------------------------------------------------------------------------

    /// The slot of `screen` while it plays its clip there (not paused, stopping or before its first frame).
    [[nodiscard]] std::optional<std::size_t> playingSlot(const ScreenState& screen) const {
        if (!screen.assignment.playing || !screen.assignment.slot) {
            return std::nullopt;
        }
        const Slot& slot = slots[*screen.assignment.slot];
        if (!slot.playback || slot.draining || slot.paused || !slot.frameShown || slot.clip != screen.clip) {
            return std::nullopt;
        }
        return screen.assignment.slot;
    }

    /// The audible screen (chooseAudible): `wished`, among the screens wanting a clip (before the scheduler's
    /// decisions); otherwise among those playing one. None while the eye is outside the bound room, without an
    /// engine, with video.json's audio mode off, or in a frozen run.
    [[nodiscard]] std::optional<std::size_t> chooseAudibleScreen(bool eyeInRoom, bool wished) const {
        if (options.audio == nullptr || !eyeInRoom || options.freezePtsSeconds ||
            options.video.audio.mode == AudioMode::Off) {
            return std::nullopt;
        }
        std::vector<AudibleCandidate> candidates;
        candidates.reserve(active.size());
        for (const ScreenState* screen : active) {
            const bool playing = wished ? !screen->clip.empty() : playingSlot(*screen).has_value();
            candidates.push_back(AudibleCandidate{screen->binding.objectId, screen->screenClass, screen->distance,
                                                  screen->visible, playing});
        }
        // The screen heard so far keeps the sound against one barely nearer (chooseAudible's hysteresis).
        return chooseAudible(candidates, options.video.proximityMeters, lastAudibleScreen);
    }

    /// The slot the audible screen plays on; none in silence.
    [[nodiscard]] std::optional<std::size_t> audibleSlot() const {
        return audibleScreen && *audibleScreen < active.size() ? playingSlot(*active[*audibleScreen]) : std::nullopt;
    }

    /// The sound's frame: the audible screen chosen, each slot's source given its gain, pan and clock, each decoder's
    /// sound decoded only while it may be heard; the changes, levels and clips recorded.
    void updateAudio(bool eyeInRoom) {
        if (options.audio == nullptr) {
            return;
        }
        audio::Mixer& mixer = options.audio->mixer();
        audibleScreen = chooseAudibleScreen(eyeInRoom, false);
        const std::optional<std::size_t> heard = audibleSlot();
        const ScreenState* audible = audibleScreen ? active[*audibleScreen] : nullptr;
        const SpatialSound spatial =
            audible != nullptr ? spatialSound(audible->distance, audible->azimuth, options.video.proximityMeters)
                               : SpatialSound{};
        // The changes of the audible screen.
        const std::string audibleId = audible != nullptr ? audible->binding.objectId : std::string();
        const std::string audibleGame = audible != nullptr ? audible->policy.targetId() : std::string();
        if (!audibleRecorded || audibleId != lastAudibleScreen || audibleGame != lastAudibleGame) {
            audibleRecorded = true;
            lastAudibleScreen = audibleId;
            lastAudibleGame = audibleGame;
            if (audioCounters.audible.size() < kMaxAudioTimelineEntries) {
                audioCounters.audible.push_back(AudioAudibleRecord{frameIndex, now, audibleId, audibleGame});
            }
            spdlog::info("Audio: {}", audible != nullptr
                                          ? fmt::format("{} is heard (game {})", nameOf(*audible), audibleGame)
                                          : std::string("no screen is heard"));
        }
        if (audible != nullptr && heard && now >= nextLevelAt) {
            nextLevelAt = now + kAudioLevelSampleSeconds;
            if (audioCounters.levels.size() < kMaxAudioTimelineEntries) {
                audioCounters.levels.push_back(AudioLevelRecord{now, audibleId, audible->distance, spatial.gain,
                                                                spatial.pan, audible->volume * spatial.gain});
            }
        }
        const double stamp = options.audio->clockSeconds(now);
        for (std::size_t index = 0; index < slots.size(); ++index) {
            Slot& slot = slots[index];
            if (!slot.source || !slot.playback || slot.draining) {
                continue;
            }
            const bool isHeard = heard == index;
            const bool playing = slot.frameShown && !slot.paused && !options.freezePtsSeconds;
            audio::SourceControl control;
            control.playing = playing;
            control.gain = isHeard ? audible->volume * spatial.gain : 0.0;
            control.pan = isHeard ? spatial.pan : 0.0;
            mixer.setControl(*slot.source, control);
            const media::PresentationClock& clock = slot.playback->clock();
            mixer.setClock(*slot.source, audio::SourceClock{clock.mediaTime(now), stamp, clock.generation(), playing});
            // Its sound is decoded while it is heard, about to be (its screen's first frame still to come) or still
            // fading out; otherwise its packets are discarded.
            slot.playback->decoder().setAudioEnabled(isHeard || mixer.sourceGain(*slot.source) > 0.0 ||
                                                     (!slot.frameShown && slot.clip == intendedAudibleClip));
            if (isHeard && !slot.audioRecorded) {
                if (const std::optional<media::StreamInfo> info = slot.playback->decoder().streamInfo()) {
                    slot.audioRecorded = true;
                    if (audioCounters.clips.size() < kMaxAudioTimelineEntries) {
                        audioCounters.clips.push_back(AudioClipRecord{
                            slot.gameId, info->audioCodec, static_cast<std::uint64_t>(info->audioSampleRate),
                            static_cast<std::uint64_t>(info->audioChannels)});
                    }
                }
            }
            if (isHeard && options.lockstep && playing) {
                awaitSound(slot);
            }
        }
    }

    /// A lockstep run: the audible clip's sound decoded kAudioLeadSeconds past its picture's time (at most kAudioWait),
    /// so that the frame's mix does not depend on the decoder thread's pace.
    void awaitSound(Slot& slot) {
        const std::optional<double> media = slot.playback->clock().mediaTime(now);
        const std::optional<media::StreamInfo> info = slot.playback->decoder().streamInfo();
        if (!media || !slot.ring || !info || !info->audioDecoded) {
            return;
        }
        const Clock::time_point deadline = Clock::now() + kAudioWait;
        for (;;) {
            const std::optional<audio::RingFront> back = slot.ring->back();
            if (back && back->ptsSeconds >= *media + kAudioLeadSeconds) {
                return;
            }
            // Nothing more comes while the decoder stops, or waits for room in its frame queue (a clip whose sound is
            // muxed after its picture: the picture's frames are what it waits on).
            const media::VideoDecoder& decoder = slot.playback->decoder();
            if (decoder.state() != media::DecoderState::Playing || Clock::now() >= deadline ||
                decoder.frames().size() >= decoder.frames().capacity()) {
                return;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
    }

    void launchStarting() {
        if (phase != ScreenPhase::Running) {
            return;
        }
        VideoLaunchRecord stop;
        stop.decoderRan = std::any_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.playback != nullptr; });
        // Each screen (and cabinet: I5 step 4) keeps its clip's position for the return.
        for (ScreenState* screen : allScreens()) {
            std::optional<double> position;
            if (screen->videoTold && screen->lastSlot) {
                const Slot& slot = slots[*screen->lastSlot];
                if (slot.playback && !slot.draining && slot.clip == screen->clip) {
                    position = clipPosition(slot);
                }
            }
            screen->policy.launchStarting(position);
            if (isHero(*screen)) {
                stop.positionSeconds = position;
            }
        }
        for (const SlotCommand& command : scheduler.stopAll("launch")) {
            stopSlot(command.slot, command.reason);
        }
        const Clock::time_point start = Clock::now();
        const auto bound = std::chrono::duration<double>(kScreenStopBoundSeconds);
        while (decoderThreads() > 0 && Clock::now() - start < bound) {
            std::this_thread::sleep_for(kPollInterval);
        }
        stop.stopMs = stop.decoderRan ? millisecondsSince(start) : 0.0;
        stop.threadsRunning = decoderThreads();
        reapDrained();
        phase = ScreenPhase::Launched;
        if (stop.threadsRunning > 0) {
            spdlog::warn("Video: {} decoder thread(s) did not end within {:g} s; the launch goes on (they end on their "
                         "own)",
                         stop.threadsRunning, kScreenStopBoundSeconds);
        } else if (stop.decoderRan) {
            spdlog::info("Video: the decoders stopped for the launch in {:.1f} ms", stop.stopMs);
        }
        counters.launches.push_back(stop);
    }

    void launchEnded() {
        if (phase != ScreenPhase::Launched) {
            return;
        }
        phase = ScreenPhase::Running;
        for (ScreenState* screen : allScreens()) {
            screen->policy.launchEnded();
        }
        reopenPending = true;
    }

    /// Lets go of the screens: the clips stop (never waited for), the stills go, the overrides cleared when asked.
    void unbind(bool clearOverride) {
        for (const SlotCommand& command : scheduler.stopAll("unbind")) {
            stopSlot(command.slot, command.reason);
        }
        resumeAt.clear();
        const bool draining = std::any_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.draining; });
        for (const std::unique_ptr<ScreenState>& screen : screens) {
            pool.cancel(screen->stillJobs);
            if (clearOverride && screen->appliedOverride != 0) {
                if (auto cleared =
                        materials.setTextureOverride(screen->binding.material, gl::MaterialTextureSlot::BaseColor, 0);
                    !cleared) {
                    spdlog::error("{}: the screen's picture cannot be cleared: {}", nameOf(*screen),
                                  cleared.error().toString());
                }
            }
            if (clearOverride && !isWholeScreen(screen->rect)) {
                // Its own pattern fills the screen again (I5 step 4).
                static_cast<void>(materials.setPictureRect(screen->binding.material, std::nullopt));
            }
            spdlog::info("{}: the screen of '{}' let go{}", nameOf(*screen), screen->binding.objectId,
                         draining ? "; its clip's decoder ends on its own" : "");
        }
        sparePolicies.clear();
        for (std::unique_ptr<ScreenState>& screen : screens) {
            sparePolicies.push_back(std::move(screen->policy));
        }
        screens.clear();
        dropCabinets();
    }

    // --- the arcade cabinets (I5 step 4, ADR-0016 §6) ---------------------------------------------------------------

    /// Records a cabinet's screen starting or ending to show its clip; `between` frames (setCabinets() before the
    /// frame's update(), an unbind) on the next frame's update(), the frame whose view shows it.
    void recordCabinet(const ScreenState& cabinet, std::string_view picture, bool between = false) {
        VideoCabinetEventRecord event{frameIndex,
                                      now,
                                      cabinet.binding.objectId,
                                      cabinet.policy.targetId(),
                                      std::string(toString(cabinet.screenClass)),
                                      std::string(picture)};
        if (between) {
            pendingCabinetEvents.push_back(std::move(event));
        } else if (counters.cabinetEvents.size() < kMaxTimelineEntries) {
            counters.cabinetEvents.push_back(std::move(event));
        } else {
            ++counters.cabinetEventsDropped;
        }
        spdlog::info("{}: {} of game {}", nameOf(cabinet), picture == "video" ? "its clip shows" : "its cover again",
                     cabinet.policy.targetId());
    }

    /// The events recorded between frames, stamped with this one.
    void flushCabinetEvents() {
        for (VideoCabinetEventRecord& event : pendingCabinetEvents) {
            event.frame = frameIndex;
            event.atSeconds = now;
            if (counters.cabinetEvents.size() < kMaxTimelineEntries) {
                counters.cabinetEvents.push_back(std::move(event));
            } else {
                ++counters.cabinetEventsDropped;
            }
        }
        pendingCabinetEvents.clear();
    }

    /// Every cabinet lets its clip go (the scheduler stops it at its next frame: "unwanted"), those showing it record
    /// their covers.
    void dropCabinets() {
        for (const std::unique_ptr<ScreenState>& cabinet : cabinets) {
            if (cabinet->shownPicture == ScreenPicture::Video) {
                recordCabinet(*cabinet, "cover", true);
            }
        }
        cabinets.clear();
        shownCabinets.clear();
        active = allScreens();
        audibleScreen.reset();
    }

    void setCabinets(std::vector<CabinetScreen> requests, CabinetPool given) {
        cabinetPool = std::move(given);
        if (screens.empty()) {
            dropCabinets(); // unbound: the room's games unknown, none plays
            return;
        }
        // During a launch no new cabinet is made (makeCabinet()); one asking through it, its position kept, stays.
        std::vector<std::unique_ptr<ScreenState>> next;
        next.reserve(requests.size());
        for (CabinetScreen& request : requests) {
            const auto found = std::find_if(cabinets.begin(), cabinets.end(), [&](const auto& cabinet) {
                return cabinet && cabinet->binding.objectId == request.id && cabinet->policy.targetId() == request.gameId;
            });
            std::unique_ptr<ScreenState> cabinet;
            if (found != cabinets.end()) {
                cabinet = std::move(*found);
            } else {
                cabinet = makeCabinet(request);
                if (!cabinet) {
                    continue;
                }
            }
            cabinet->binding.bounds = request.bounds;
            cabinet->binding.screenSize = cabinetPool.screenSize;
            cabinet->screenClass = request.screenClass;
            cabinet->inHand = request.inHand;
            next.push_back(std::move(cabinet));
        }
        // Those no longer asked for let their clips go.
        for (const std::unique_ptr<ScreenState>& cabinet : cabinets) {
            if (cabinet && cabinet->shownPicture == ScreenPicture::Video) {
                recordCabinet(*cabinet, "cover", true);
            }
        }
        cabinets = std::move(next);
        active = allScreens();
        audibleScreen.reset();
    }

    /// A cabinet's screen for `request`, playing its game's clip from the bound room's games (as the verdicts screen
    /// them); none for a game the room lacks, one whose clip failed on a cabinet in the run, or during a launch.
    [[nodiscard]] std::unique_ptr<ScreenState> makeCabinet(const CabinetScreen& request) {
        const ScreenState* first = hero();
        if (first == nullptr || phase != ScreenPhase::Running || failedCabinetGames.contains(request.gameId)) {
            return nullptr;
        }
        const std::span<const ScreenGame> games = first->policy.games();
        const auto game = std::find_if(games.begin(), games.end(),
                                       [&](const ScreenGame& candidate) { return candidate.id == request.gameId; });
        if (game == games.end()) {
            return nullptr;
        }
        ScreenGame own = *game;
        own.startCandidate = true;
        ScreenBinding binding;
        binding.objectId = request.id;
        binding.bounds = request.bounds;
        binding.roomId = first->binding.roomId;
        binding.kind = ScreenKind::Cabinet;
        binding.screenSize = cabinetPool.screenSize;
        binding.followsWorldSelection = false;
        // Its own game, from its start, looping: no playlist, nothing selected (video.json's cabinet keys decide whether
        // it asks at all: app::chooseCabinets).
        ScreenPlaySettings settings = screenPlaySettings(options, binding);
        settings.autoplay = true;
        settings.showOnFocus = false;
        settings.startChoice = true;
        settings.previewSeconds = kMaxPreviewSeconds;
        settings.playlist.explicitGames.clear();
        const double volume = options.video.screen(ScreenKind::Cabinet, binding.roomId, binding.objectId).volume;
        auto cabinet = std::make_unique<ScreenState>(std::move(binding), ScreenPolicy({std::move(own)}, settings));
        cabinet->cabinet = true;
        cabinet->volume = volume;
        cabinet->screenClass = request.screenClass;
        cabinet->shownPicture = ScreenPicture::Cover; // its item's own front
        return cabinet;
    }

    /// The cabinets whose clips show (a frame of it in their slot's texture) get their slot's pooled material: its base
    /// colour the slot's texture, its picture rectangle the clip's on the cabinet's screen; the others show their
    /// covers. The changes are recorded.
    void applyCabinets() {
        std::vector<CabinetShown> shown;
        for (const std::unique_ptr<ScreenState>& cabinet : cabinets) {
            followSlot(*cabinet);
            const bool video = cabinet->policy.picture() == ScreenPicture::Video && cabinet->lastSlot &&
                               *cabinet->lastSlot < cabinetPool.materials.size() &&
                               slots[*cabinet->lastSlot].texture.valid();
            if (!video) {
                if (cabinet->shownPicture == ScreenPicture::Video) {
                    recordCabinet(*cabinet, "cover");
                }
                cabinet->shownPicture = ScreenPicture::Cover;
                cabinet->appliedOverride = 0;
                continue;
            }
            const std::size_t index = *cabinet->lastSlot;
            const Slot& slot = slots[index];
            const scene::MaterialHandle material = cabinetPool.materials[index];
            const GLuint texture = slot.texture.textureId();
            if (materials.textureOverride(material, gl::MaterialTextureSlot::BaseColor) != texture) {
                if (auto set = materials.setTextureOverride(material, gl::MaterialTextureSlot::BaseColor, texture);
                    !set) {
                    spdlog::error("{}: the screen's picture cannot be set: {}", nameOf(*cabinet),
                                  set.error().toString());
                    continue;
                }
            }
            const glm::vec4 rect = pictureRect(glm::vec2(slot.texture.size()), cabinetPool.screenSize);
            if (auto done =
                    materials.setPictureRect(material, isWholeScreen(rect) ? std::nullopt : std::optional(rect));
                !done) {
                spdlog::error("{}: the picture's rectangle cannot be set: {}", nameOf(*cabinet),
                              done.error().toString());
            }
            cabinet->rect = rect;
            cabinet->appliedOverride = texture;
            cabinet->overrideSlot = index;
            if (cabinet->shownPicture != ScreenPicture::Video) {
                cabinet->shownPicture = ScreenPicture::Video;
                recordCabinet(*cabinet, "video");
            }
            shown.push_back(CabinetShown{cabinet->binding.objectId, cabinet->policy.targetId(), material});
        }
        shownCabinets = std::move(shown);
    }

    void bind(std::vector<ScreenBinding> bindings, std::vector<ScreenGame> games, std::uint64_t frame) {
        const Clock::time_point start = Clock::now();
        rawGames = games;
        games = verdicts.screened(std::move(games));
        // A clip open on the screens let go (playing, or paused out of view): stopped below, never waited for.
        const bool clipStopped =
            std::any_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.playback && !slot.draining; });
        if (!screens.empty()) {
            unbind(true);
        }
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            ScreenBinding& binding = bindings[index];
            ScreenPlaySettings settings = screenPlaySettings(options, binding);
            // A policy let go is reused (its phase and the clips that failed kept), its choice made afresh.
            ScreenPolicy policy = index < sparePolicies.size() ? std::move(sparePolicies[index]) : ScreenPolicy();
            policy.restart(games, std::move(settings));
            const double volume = options.video.screen(binding.kind, binding.roomId, binding.objectId).volume;
            screens.push_back(std::make_unique<ScreenState>(std::move(binding), std::move(policy)));
            screens.back()->volume = volume;
            logPlaylist(*screens.back());
        }
        sparePolicies.clear();
        reapDrained();
        if (ScreenState* first = hero()) {
            lastScreenId = first->binding.objectId;
        }
        VideoBindRecord record;
        record.frame = frame;
        record.screen = lastScreenId;
        record.games = hero() != nullptr ? hero()->policy.games().size() : 0;
        record.bindMs = millisecondsSince(start);
        record.threadsRunning = decoderThreads();
        record.clipStopped = clipStopped;
        spdlog::info("Video: bound to the {} screen(s) of '{}' with {} game(s) in {:.2f} ms ({}{} decoder thread(s) "
                     "ending)",
                     screens.size(), record.screen, record.games, record.bindMs,
                     clipStopped ? "its clips stopped, " : "", record.threadsRunning);
        counters.binds.push_back(std::move(record));
    }

    void shutdown() {
        flushCabinetEvents(); // I5 step 4: a cabinet let go since the last frame
        // The run's verdicts reach the file (I5 step 7); idempotent.
        verdicts.shutdown(kStillWait);
        const bool decoders =
            std::any_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.playback != nullptr; });
        if (phase == ScreenPhase::Quit && !decoders) {
            return;
        }
        // What the run ended with, for the report: the phase (launched when quitting during a game) and the decoder.
        if (!endPhase) {
            endPhase = std::string(toString(phase));
            endDecoderState = decoderStateName();
        }
        phase = ScreenPhase::Quit;
        for (const std::unique_ptr<ScreenState>& screen : screens) {
            screen->policy.quit();
        }
        for (const SlotCommand& command : scheduler.stopAll("shutdown")) {
            stopSlot(command.slot, command.reason);
        }
        // Every stopped decoder's thread (ADR-0004: at shutdown, never from the render loop): the waits made before
        // this one, which the report shows to be none.
        counters.waitsBeforeExit = media::stoppedDecoderWaits();
        media::waitForStoppedDecoders();
        reapDrained();
        counters.joinedAtExit = decoderThreads() == 0;
        pool.shutdown();
    }
};

ScreenService::ScreenService(std::vector<ScreenBinding> screens, gl::MaterialRegistry& materials,
                             std::vector<ScreenGame> games, ScreenServiceOptions options)
    : m_impl(std::make_unique<Impl>(std::move(screens), materials, std::move(games), std::move(options))) {
    const Impl& s = *m_impl;
    spdlog::info("Video: {} screen(s) from '{}', {} game(s) with media, {} decoder(s) at most{}", s.screens.size(),
                 s.lastScreenId, s.hero() != nullptr ? s.hero()->policy.games().size() : 0,
                 s.scheduler.settings().maxDecoders,
                 s.options.freezePtsSeconds ? fmt::format(", videos held at {:g} s", *s.options.freezePtsSeconds)
                                            : std::string());
    for (const std::unique_ptr<Impl::ScreenState>& screen : m_impl->screens) {
        m_impl->logPlaylist(*screen);
    }
}

ScreenService::~ScreenService() {
    if (!m_impl) {
        return;
    }
    m_impl->shutdown();
    // The textures go with the service: the materials draw their own pictures again.
    for (const std::unique_ptr<Impl::ScreenState>& screen : m_impl->screens) {
        if (screen->appliedOverride != 0) {
            static_cast<void>(m_impl->materials.setTextureOverride(screen->binding.material,
                                                                   gl::MaterialTextureSlot::BaseColor, 0));
        }
        if (!isWholeScreen(screen->rect)) {
            static_cast<void>(m_impl->materials.setPictureRect(screen->binding.material, std::nullopt));
        }
    }
    for (Impl::Slot& slot : m_impl->slots) {
        m_impl->releaseDecoder(slot);
    }
}

void ScreenService::setGames(std::vector<ScreenGame> games) {
    m_impl->rawGames = games;
    games = m_impl->verdicts.screened(std::move(games));
    for (const std::unique_ptr<Impl::ScreenState>& screen : m_impl->screens) {
        screen->policy.setGames(games);
    }
    m_impl->dropCabinets(); // asked for again next frame, with the new games' media
}

std::size_t ScreenService::slotCount() const noexcept {
    return m_impl->slots.size();
}

void ScreenService::setCabinets(std::vector<CabinetScreen> cabinets, CabinetPool cabinetPool) {
    if (m_impl->phase == ScreenPhase::Quit) {
        return;
    }
    m_impl->setCabinets(std::move(cabinets), std::move(cabinetPool));
}

std::span<const CabinetShown> ScreenService::cabinetsShown() const noexcept {
    return m_impl->shownCabinets;
}

void ScreenService::unbind(bool clearOverride) {
    m_impl->unbind(clearOverride);
}

void ScreenService::bind(std::vector<ScreenBinding> screens, std::vector<ScreenGame> games, std::uint64_t frame) {
    m_impl->bind(std::move(screens), std::move(games), frame);
}

bool ScreenService::bound() const noexcept {
    return !m_impl->screens.empty();
}

std::string ScreenService::screenId() const {
    const Impl::ScreenState* hero = m_impl->hero();
    return hero != nullptr ? hero->binding.objectId : std::string();
}

void ScreenService::update(std::uint64_t frame, double nowSeconds, const scene::Camera& camera,
                           const ScreenSelection& selection, bool minimized, bool eyeInRoom) {
    m_impl->update(frame, nowSeconds, camera, selection, minimized, eyeInRoom);
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
    const Impl::ScreenState* hero = s.hero();
    record.screen = hero != nullptr ? hero->binding.objectId : s.lastScreenId;
    record.phase = s.endPhase.value_or(std::string(toString(s.phase)));
    record.decoderState = s.endDecoderState.value_or(s.decoderStateName());
    record.freezePtsSeconds = s.options.freezePtsSeconds;
    if (hero != nullptr) {
        record.gameId = hero->policy.targetId();
        record.picture = std::string(toString(hero->shownPicture));
        record.selections = hero->policy.selections();
        record.playlistMode = std::string(toString(hero->policy.playlist().mode()));
        record.playlistRoom = hero->policy.settings().playlist.roomId;
        record.previewSeconds = hero->policy.settings().previewSeconds;
        record.playlistOrder = hero->policy.playlist().order();
        record.origin = std::string(toString(hero->policy.origin()));
    } else {
        record.picture = std::string(toString(ScreenPicture::IdlePattern));
    }
    if (const Impl::Slot* slot = s.heroClipSlot()) {
        record.frozenFrameShown = slot->frozenShown;
        record.firstFrameMs = slot->firstFrameMs;
        record.clipFramesUploaded = slot->frames;
        record.firstPtsSeconds = slot->firstPts;
        record.lastPtsSeconds = slot->lastPts;
        record.clipUploadSeconds = slot->firstUploadAt ? slot->lastUploadAt - *slot->firstUploadAt : 0.0;
    }
    record.videoBytes = 0;
    for (const Impl::Slot& slot : s.slots) {
        record.videoBytes += slot.texture.valid() ? slot.texture.gpuBytes() : 0;
    }
    record.stillBytes = 0;
    for (const Impl::ScreenState* screen : s.allScreens()) {
        record.stillBytes += (screen->still.valid() ? screen->still.estimatedBytes() : 0) +
                             (screen->heldStill.valid() ? screen->heldStill.estimatedBytes() : 0) +
                             (screen->heldFrame.valid() ? screen->heldFrame.estimatedBytes() : 0);
        VideoScreenRecord entry;
        entry.screen = screen->binding.objectId;
        entry.kind = std::string(toString(screen->binding.kind));
        entry.screenClass = std::string(toString(screen->screenClass));
        entry.gameId = screen->policy.targetId();
        entry.picture = std::string(toString(screen->shownPicture));
        if (screen->assignment.slot) {
            entry.slot = *screen->assignment.slot;
        }
        entry.playing = screen->assignment.playing;
        entry.textureId = screen->appliedOverride;
        entry.visible = screen->visible;
        if (!isWholeScreen(screen->rect)) {
            entry.pictureRect = {screen->rect.x, screen->rect.y, screen->rect.z, screen->rect.w};
        }
        entry.heldFrame = screen->showingHeldFrame;
        record.screens.push_back(std::move(entry));
    }
    record.maxDecoders = static_cast<std::uint64_t>(s.scheduler.settings().maxDecoders);
    record.handOvers = s.scheduler.handOvers();
    for (const BudgetDrop& drop : s.scheduler.drops()) {
        const auto game = s.clipGames.find(drop.clip);
        record.budgetDrops.push_back(VideoBudgetDropRecord{
            drop.atSeconds, drop.slot, game != s.clipGames.end() ? game->second : std::string(), drop.ratio,
            drop.untilSeconds});
    }
    s.verdicts.record(record.verdicts);
}

void ScreenService::recordAudio(AudioRecord& record) const {
    const Impl& s = *m_impl;
    record.audible = s.audioCounters.audible;
    record.levels = s.audioCounters.levels;
    record.presented = s.audioCounters.presented;
    record.clips = s.audioCounters.clips;
    record.overflowFrames = s.pastOverflowFrames;
    for (const Impl::Slot& slot : s.slots) {
        if (slot.ring) {
            record.overflowFrames += slot.ring->stats().overflowFrames;
        }
    }
}

std::vector<std::string> ScreenService::statusLines() const {
    const Impl& s = *m_impl;
    std::vector<std::string> lines;
    lines.push_back(fmt::format("Screens: {}; decoders: {} open of {} (budget {}), {} thread(s); {} started, at most {} "
                                "at once",
                                s.screens.size(), s.scheduler.openSlots(), s.slots.size(), s.scheduler.budget(s.now),
                                s.decoderThreads(), s.counters.decodersStarted, s.counters.maxOpenDecoders));
    for (const Impl::ScreenState* screen : s.allScreens()) {
        lines.push_back(fmt::format(
            "{} '{}': {}{}{}{}", s.isHero(*screen) ? "Hero" : screen->cabinet ? "Cabinet" : "Screen",
            screen->binding.objectId,
            toString(screen->shownPicture),
            screen->policy.targetId().empty() ? std::string() : " of " + screen->policy.targetId(),
            screen->assignment.slot ? fmt::format(", slot {}{}", *screen->assignment.slot,
                                                  screen->assignment.waiting ? " (waiting)" : "")
                                    : std::string(", no slot"),
            screen->visible ? "" : " (out of view)"));
    }
    for (std::size_t index = 0; index < s.slots.size(); ++index) {
        const Impl::Slot& slot = s.slots[index];
        lines.push_back(fmt::format(
            "Slot {}: {}{}{}", index, toString(s.scheduler.slots()[index].state),
            slot.playback ? fmt::format(", game {}, {}", slot.gameId, media::toString(slot.playback->decoder().state()))
                          : std::string(),
            slot.lastPts ? fmt::format(", {:.3f} s, {} frame(s)", *slot.lastPts, slot.frames) : std::string()));
    }
    lines.push_back(fmt::format("Uploads: {} frame(s), at most {} bytes in a frame, {} deferred, ring busy {}",
                                s.counters.framesUploaded, s.counters.maxUploadBytes, s.counters.deferredUploads,
                                s.counters.ringBusy));
    if (const Impl::ScreenState* hero = s.hero()) {
        lines.push_back(fmt::format("Playlist: {} of {} ({} game(s), cycle {}), {} ({:.1f} s played)",
                                    toString(hero->policy.playlist().mode()), hero->policy.settings().playlist.roomId,
                                    hero->policy.playlist().size(), hero->policy.playlist().cycle(),
                                    toString(hero->policy.origin()), hero->policy.targetSeconds()));
    }
    lines.push_back(
        fmt::format("Fallbacks: {}; launches: {}", s.counters.fallbacks.size(), s.counters.launches.size()));
    return lines;
}

} // namespace rv::app
