#pragma once

#include <rv/app/screen_policy.hpp>
#include <rv/app/screen_scheduler.hpp>
#include <rv/app/video_settings.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/render_item.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// A screen of a scene the service shows pictures on: its object, the material its screen is drawn with (a picture
/// instance of MAT_Screen_CRT, whose base colour texture the service overrides), its world bounds (the view test that
/// pauses its decoder) and its room's id (video.json's rooms, the playlist's seeds): the room of a floor ("room:snes"),
/// else the scene's name (the test room's "m0_testroom"); since I5 step 2 its kind (the hero, or a secondary CRT).
struct ScreenBinding {
    std::string objectId;
    scene::MaterialHandle material;
    core::Aabb bounds;
    std::string roomId;
    ScreenKind kind = ScreenKind::Hero;
};

/// The screens of `description`, the hero first: the first object built from the `crt` prefab, or from the
/// `arcade_cabinet` prefab that stands for it in the arcade (I6 step 6's hero cabinet), whose screen shows a picture
/// (its "screen" role has a scene texture, so that its material has a base colour texture to override), with its
/// screen's material in `materials` (registered by SceneInstance under "<object id>/screen...") and the bounds of its
/// first instance; then (I5 step 2) every other `crt` object with a picture, as a secondary screen (in a scene with
/// areas, only those in the hero's area). Empty when the scene has none (the model viewer, scenes without a TV). With
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
    /// --fixed-dt (lockstep runs): a still picture being loaded is waited for (at most kStillWait), so that scripted
    /// runs show the same picture on the same frame.
    bool lockstep = false;
    /// video.json (I5 step 1, ADR-0016 §4): clips at all, each room's and screen's settings (its playlist,
    /// previewDuration, showOnFocus, the explicit games); since step 2 the budget, the rows and proximityMeters.
    VideoSettings video;
    /// --max-decoders (I5 step 2): the budget over video.json's maxDecoders.
    std::optional<int> maxDecoders;
    /// The asset root: the title cards' bundled fonts.
    std::filesystem::path assetRoot;
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

    /// The room's games changed (ScreenPolicy::setGames on every screen).
    void setGames(std::vector<ScreenGame> games);

    /// Lets go of the screens (I4 step 6b): the clips stop (never waited for), the stills go, and the screens' picture
    /// overrides clear when `clearOverride` (false: the scene's registry was cleared, the materials are gone).
    /// Unbound, the service shows nothing and opens no decoder until bind().
    void unbind(bool clearOverride);
    /// Binds `screens` (another room's, or the same room's in a scene committed again) with `games`, each screen's
    /// choice made as at the start; recorded with `frame` (VideoRecord::binds).
    void bind(std::vector<ScreenBinding> screens, std::vector<ScreenGame> games, std::uint64_t frame);
    /// Screens are bound, and the hero's object id ("" none).
    [[nodiscard]] bool bound() const noexcept;
    [[nodiscard]] std::string screenId() const;

    /// One frame (`frame`, at `nowSeconds`: the run's simulated time) seen by `camera`: the item focused or inspected
    /// (`hovered`, its catalog id), the window `minimized` (every screen out of view); the scheduler's decisions
    /// carried out, the frames uploaded, the stills loaded, the screens' texture overrides set to what they show.
    void update(std::uint64_t frame, double nowSeconds, const scene::Camera& camera,
                std::optional<std::string_view> hovered, bool minimized = false);

    /// A launch starts (before the window's hand-over): every decoder stops and its thread is waited for (bounded).
    void launchStarting();
    /// The launch ended, failed to start or was refused: the clips open again at their positions.
    void launchEnded();
    /// The end of the run: the decoders stay stopped, every decoder thread is waited for, the worker ends.
    void shutdown();

    /// The run report's video section.
    void record(VideoRecord& record) const;
    /// Lines for the developer overlay's Stats panel.
    [[nodiscard]] std::vector<std::string> statusLines() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
