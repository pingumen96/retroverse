#pragma once

#include <rv/app/screen_policy.hpp>
#include <rv/app/screen_scheduler.hpp>
#include <rv/app/video_settings.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/render_item.hpp>

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rv::audio {
class AudioEngine;
} // namespace rv::audio

namespace rv::gl {
class MaterialRegistry;
} // namespace rv::gl

namespace rv::scene {
class Camera;
struct SceneDescription;
} // namespace rv::scene

namespace rv::app {

class SceneInstance;
struct VideoRecord;
struct AudioRecord;

/// A screen of a scene the service shows pictures on: its object, the material its screen is drawn with (a picture
/// instance of MAT_Screen_CRT, whose base colour texture the service overrides), its world bounds (the view test that
/// pauses its decoder) and its room's id (video.json's rooms, the playlist's seeds): the room of a floor ("room:snes"),
/// else the scene's name (the test room's "m0_testroom"); since I5 step 2 its kind (the hero, or a secondary CRT); since
/// I5 step 4 its screen's size (its ScreenSurface's width and height: a clip of another aspect is letterboxed or
/// pillarboxed, ADR-0016 §7; zero: unknown, the whole screen) and whether it follows the world's selection (the
/// arcade's hero cabinet does not: the focused or inspected cabinet plays its clip itself, ADR-0016 §6; it follows the
/// quick catalog's, as every screen does).
struct ScreenBinding {
    std::string objectId;
    scene::MaterialHandle material;
    core::Aabb bounds;
    std::string roomId;
    ScreenKind kind = ScreenKind::Hero;
    glm::vec2 screenSize{0.0f};
    bool followsWorldSelection = true;
};

/// The games selected on a frame (catalog ids): the item in the world's Focus or Inspect (WorldSession's), and the
/// quick catalog's while it is open (a game of the eye's room). A screen follows the quick catalog's, else the
/// world's; the arcade's hero cabinet only the quick catalog's (ScreenBinding::followsWorldSelection).
struct ScreenSelection {
    std::optional<std::string_view> world;
    std::optional<std::string_view> catalog;
};

/// An arcade cabinet asking for its own clip on this frame (I5 step 4, ADR-0016 §6; app::chooseCabinets): its screen's
/// id (app::cabinetScreenId(): the scheduler's screen and owner), its game (one of the bound room's), its class (`focus`,
/// `direct` when inspected, `ambient` in attract), its screen's box in the scene (the view test, the distance to the
/// eye), and whether it is held in the hand (inspected: in view, at arm's length, whatever its slot's box).
struct CabinetScreen {
    std::string id;
    std::string gameId;
    ScreenClass screenClass = ScreenClass::Focus;
    core::Aabb bounds;
    bool inHand = false;

    bool operator==(const CabinetScreen&) const = default;
};

/// The pooled video materials of the cabinets' collection (I5 step 4: CollectionInstance::videoMaterials(), one per
/// decoder slot, slot i's picture on material i) and the size of its screens (the family's label: the clip's aspect is
/// kept within it).
struct CabinetPool {
    std::vector<scene::MaterialHandle> materials;
    glm::vec2 screenSize{0.0f};
};

/// A cabinet whose screen shows its clip on this frame: its screen's id, its game and the pooled material that draws it
/// (its base colour the slot's texture, its picture rectangle the clip's).
struct CabinetShown {
    std::string id;
    std::string gameId;
    scene::MaterialHandle material;

    bool operator==(const CabinetShown&) const = default;
};

/// The prefab kind of a pack's secondary TV (world::kSecondaryTvKind, I5 step 3).
inline constexpr std::string_view kSecondaryTvPrefabKind = "secondary_tv";
/// The id's end of a generated room's hero CRT, the hero corner's "crt" part ("room:snes/fx:hero.crt"), which the
/// arcade's hero cabinet keeps.
inline constexpr std::string_view kHeroCrtIdSuffix = ".crt";

/// The screens of `description`, the hero first: by its role since I5 step 3, the object whose id ends with
/// kHeroCrtIdSuffix built from the `crt` prefab, or from the `arcade_cabinet` prefab that stands for it in the arcade
/// (I6 step 6's hero cabinet, which never follows the world's selection: I5 step 4), else the first `crt` (a hand-made
/// scene's), whose screen shows a picture (its "screen" role has a scene texture, so that its material has a base
/// colour texture to override), with its screen's material in `materials` (registered by SceneInstance under
/// "<object id>/screen..."), the bounds of its first instance and its ScreenSurface's size; then, as secondary screens,
/// every `secondary_tv` object with a picture (I5 step 3: a pack's secondary TV) and every other `crt` (I5 step 2: a
/// hand-made scene's, screens_four's), in a scene with areas only those in the hero's area. Empty when the scene has
/// no hero (the model viewer, scenes without a TV). With
/// `idPrefix` (I4 step 5a: a floor's start room, "room:snes/") only objects whose ids start with it count, and the
/// room's id is the prefix without its "/"; an empty prefix finds none. Without, the room's id is the scene's name.
[[nodiscard]] std::vector<ScreenBinding> findScreens(const scene::SceneDescription& description,
                                                     const SceneInstance& instance,
                                                     const gl::MaterialRegistry& materials,
                                                     const std::optional<std::string>& idPrefix = std::nullopt);

struct ScreenServiceOptions {
    /// --video-freeze-pts: every clip opens paused, seeks to this time and holds that frame (the frame is waited for
    /// before the frame that shows it goes on, at most kFreezeWait), for goldens.
    std::optional<double> freezePtsSeconds;
    /// --fixed-dt (lockstep runs): a still picture being loaded is waited for (at most kStillWait), and so is the first
    /// frame of a clip a slot opened for a screen in view (at most kFirstFrameWait, NFR-06's 2 s), so that scripted runs
    /// show the same picture on the same frame.
    bool lockstep = false;
    /// video.json (I5 step 1, ADR-0016 §4): clips at all, each room's and screen's settings (its playlist,
    /// previewDuration, showOnFocus, the explicit games); since step 2 the budget, the rows and proximityMeters.
    VideoSettings video;
    /// --max-decoders (I5 step 2): the budget over video.json's maxDecoders.
    std::optional<int> maxDecoders;
    /// --watch-late-frames (tests): the late-frame drop is watched in a lockstep run too. Without it a lockstep run
    /// never drops (its frames are presented on simulated time and decoded on the wall clock: their lateness says
    /// nothing).
    bool watchLateFrames = false;
    /// The asset root: the title cards' bundled fonts.
    std::filesystem::path assetRoot;
    /// The clips' sound (I5 step 5, ADR-0016 §9): the run's engine (none: --audio off, or video.json's audio mode off;
    /// the clips' audio packets are then discarded undecoded). It must outlive the service.
    audio::AudioEngine* audio = nullptr;
    /// The audible clip's frames recorded (AudioRecord::presented): runs that capture the mix.
    bool recordPresentedFrames = false;
    /// The clips' verdicts (I5 step 7, ADR-0016 §8): <user data>/cache/media/videos.json, read when the service starts
    /// (a lockstep run waits for it) and given the verdicts of the clips that fail or play through once clean; a clip
    /// known to fail is never opened (its screens show its stills). Empty: no verdicts.
    std::filesystem::path verdictsFile;
};

/// The play settings of `screen` (video.json's settings of its kind, its room and itself; the frozen runs'): the hero
/// with its start choice and the seeds' screen id "hero", a secondary with its object id and no start choice.
[[nodiscard]] ScreenPlaySettings screenPlaySettings(const ScreenServiceOptions& options, const ScreenBinding& screen);
/// The decoder budget: --max-decoders, else video.json's maxDecoders, within 1 to kMaxScreenDecoders.
[[nodiscard]] int decoderBudget(const ScreenServiceOptions& options) noexcept;

/// The screens' service (I3 step 11, ADR-0012 §7; since I5 step 2 ADR-0016 §2): every screen of the room that holds the
/// eye, each driven by its ScreenPolicy (what to play: the start choice or its playlist, the dwell, the fallback chain;
/// the launch's life), sharing the decoder slots of an app::ScreenScheduler (its budget: --max-decoders or video.json's
/// maxDecoders, at most 2; never more decoders open, nor more decoder threads alive than slots: NFR-07, asserted and
/// reported). A slot owns one media::VideoDecoder at video.json's maxRows and one gl::VideoTexture (kept for the run);
/// screens asking for one clip share its slot and texture. The uploads of one frame carry at most
/// kMaxVideoUploadBytesPerFrame, the longest pending first (the CPU section videoUpload around each). The still
/// pictures of each screen's chain (screenshot, title screen, cover) are decoded and fitted to the screen's 4:3 on a
/// worker thread and uploaded as hero textures, the chain ending at the game's generated title card
/// (assets::drawTitleCard), and whichever is shown is set as the texture override of the screen's material
/// (gl::MaterialRegistry::setTextureOverride; the idle pattern is the material's own picture). A screen without a slot
/// shows its still; one whose slot pauses keeps its frame, and is stopped after kScreenStopAfterSeconds out of view.
///
/// Before a launch every slot is stopped and its thread waited for, at most kScreenStopBoundSeconds in all (a thread
/// that does not end in time is logged and the launch goes on), and after the return the clips open again at their
/// positions; once Retroverse quits nothing starts again, and shutdown() waits for every decoder thread
/// (media::waitForStoppedDecoders()). A slot opens its next clip only once its previous decoder's thread ended (the
/// screen keeps its picture meanwhile). Files are logged by game, never by path (the user's folders). Main thread
/// only; the material registry must outlive the service.
///
/// The screen keeps the picture shown up while the next game's is not ready (its still or its first frame): a switch
/// never shows the screen's own pattern, which only a screen without a game shows (the test room's colour bars, the
/// building's standby). The hero's played games, pictures shown and frames held are recorded (VideoRecord), and every
/// screen's state and the slots' events (VideoRecord::screens, ::slotEvents).
///
/// Since I4 step 6b (ADR-0013 §9) the service follows the room that holds the eye: bind() moves it to another room's
/// screens with that room's games, the clips of the previous ones stopping (their threads end on their own, never
/// waited for on the render loop; the slots open again once they have ended), the previous screens' picture
/// overrides cleared. Any scene shown in place of another lets go of its screens first (unbind(), the scene's registry
/// being cleared) and binds the new scene's: one service for the run, and shutdown() only at its end.
///
/// Since I5 step 4 (ADR-0016 §6, §7):
///  - the arcade cabinets that ask for their own clips each frame (setCabinets(): the focused or inspected one, the
///    attract one) are screens of the scheduler too, of the bound room's games, without a material of their own: the
///    service sets their slot's picture on their collection's pooled video material and tells which show it
///    (cabinetsShown()), the world drawing them; the arcade's hero cabinet never follows the world's selection, only
///    the quick catalog's;
///  - a clip of another aspect than its screen's fills the rectangle that keeps it (screen_aspect.hpp's pictureRect(),
///    gl::MaterialRegistry::setPictureRect), its bars the tube's black;
///  - a bound screen whose slot is taken from a clip it still wants ("needed", "budget") keeps a copy of that clip's
///    frame (gl::Texture2D::copyOf) until the clip has a slot again, in place of its still.
class ScreenService {
public:
    /// The still pictures' size: the screen's 4:3, as the clips.
    static constexpr glm::ivec2 kStillSize{640, 480};

    /// `screens` (findScreens(): not empty, the hero first).
    ScreenService(std::vector<ScreenBinding> screens, gl::MaterialRegistry& materials, std::vector<ScreenGame> games,
                  ScreenServiceOptions options);
    ~ScreenService();
    ScreenService(const ScreenService&) = delete;
    ScreenService& operator=(const ScreenService&) = delete;

    /// The room's games changed (ScreenPolicy::setGames on every screen; a cabinet whose game or clip changed lets its
    /// clip go, the others play on).
    void setGames(std::vector<ScreenGame> games);

    /// Lets go of the screens (I4 step 6b): the clips stop (never waited for), the stills go, and the screens' picture
    /// overrides clear when `clearOverride` (false: the scene's registry was cleared, the materials are gone).
    /// Unbound, the service shows nothing and opens no decoder until bind().
    void unbind(bool clearOverride);
    /// Binds `screens` (another room's, or the same room's in a scene committed again) with `games`, each screen's
    /// choice made as at the start; recorded with `frame` (VideoRecord::binds).
    void bind(std::vector<ScreenBinding> screens, std::vector<ScreenGame> games, std::uint64_t frame);
    /// The bound room's screens found again after its objects were replaced in place (I5 step 3: the room editor rebuilt
    /// its static part, a secondary TV it moved standing as a new object with a new material): each screen keeps its
    /// state (its clip, slot and picture) and takes its object's new material, bounds and screen size, the picture set
    /// on the new material at the next update(). False, nothing changed, when `screens` are not the bound ones (other
    /// object ids, or none bound): bind() them.
    [[nodiscard]] bool rebind(std::vector<ScreenBinding> screens);
    /// Screens are bound, and the hero's object id ("" none).
    [[nodiscard]] bool bound() const noexcept;
    [[nodiscard]] std::string screenId() const;
    /// The decoder slots (the budget: --max-decoders or video.json's maxDecoders): the pooled video materials a
    /// cabinets' room needs.
    [[nodiscard]] std::size_t slotCount() const noexcept;

    /// The arcade cabinets that ask for their clips on the next update() (I5 step 4, ADR-0016 §6), and their room's pool
    /// of video materials. A cabinet asked for again keeps its clip; one no longer asked for lets its clip go (the
    /// scheduler stops it: "unwanted"); a game of no bound room's, without a playable clip or whose clip failed in the
    /// run plays nothing (its cover stays). Unbound, none plays.
    void setCabinets(std::vector<CabinetScreen> cabinets, CabinetPool pool);
    /// After update(): the cabinets whose screens show their clips now (a frame of it shown), each with the pooled
    /// material its slot's picture is on. The others show their covers.
    [[nodiscard]] std::span<const CabinetShown> cabinetsShown() const noexcept;

    /// One frame (`frame`, at `nowSeconds`: the run's simulated time) seen by `camera`: the games selected
    /// (`selection`: the world's focused or inspected item, the quick catalog's), the window `minimized` (every screen
    /// out of view: the slots pause, and stop after
    /// kScreenStopAfterSeconds, reason "minimized"); the scheduler's decisions carried out, the frames uploaded, the
    /// stills loaded, the screens' texture overrides set to what they show. An interactive run calls it while minimized
    /// too, after each of the host's waits (IAppLogic::onMinimized), with the waits' time.
    ///
    /// The sound (I5 step 5, ADR-0016 §9): the audible screen is the visible one playing of the best class, then the
    /// nearest, within proximityMeters (app::chooseAudible), while `eyeInRoom` (the eye in the room whose screens are
    /// bound: none in a hall); its slot's source gets its volume times its spatial gain and its pan, follows its
    /// picture's media time, and its decoder decodes its sound; the others fade out (the 300 ms cross-fade) and their
    /// decoders discard their audio packets.
    void update(std::uint64_t frame, double nowSeconds, const scene::Camera& camera,
                const ScreenSelection& selection, bool minimized = false, bool eyeInRoom = true);

    /// A launch starts (before the window's hand-over): every decoder stops and its thread is waited for (bounded).
    void launchStarting();
    /// The launch ended, failed to start or was refused: the clips open again at their positions.
    void launchEnded();
    /// The end of the run: the decoders stay stopped, every decoder thread is waited for, the worker ends.
    void shutdown();

    /// The run report's video section.
    void record(VideoRecord& record) const;
    /// The screens' part of the run report's audio section: the audible screen's changes, its levels, its frames
    /// (recordPresentedFrames) and the clips whose sound was decoded.
    void recordAudio(AudioRecord& record) const;
    /// Lines for the developer overlay's Stats panel.
    [[nodiscard]] std::vector<std::string> statusLines() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
