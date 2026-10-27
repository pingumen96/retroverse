#pragma once

#include <rv/app/screen_policy.hpp>
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

/// The hero CRT of a scene: its object, the material its screen is drawn with (a picture instance of MAT_Screen_CRT,
/// whose base colour texture the service overrides) and its world bounds (the view test that pauses the decoder);
/// since I5 step 1 its room's id (video.json's rooms, the playlist's seeds): the room of a floor ("room:snes"), else
/// the scene's name (the test room's "m0_testroom").
struct HeroScreen {
    std::string objectId;
    scene::MaterialHandle material;
    core::Aabb bounds;
    std::string roomId;
};

/// The first object of `description` built from the `crt` prefab, or from the `arcade_cabinet` prefab that stands for it
/// in the arcade (I6 step 6's hero cabinet), whose screen shows a picture (its "screen" role has a
/// scene texture, so that its material has a base colour texture to override), with its screen's material in
/// `materials` (registered by SceneInstance under "<object id>/screen...") and the bounds of its first instance;
/// nothing when the scene has none (the model viewer, scenes without a TV). With `idPrefix` (I4 step 5a: a floor's start
/// room, "room:snes/") only an object whose id starts with it counts, and the room's id is the prefix without its "/";
/// an empty prefix finds none. Without, the room's id is the scene's name.
[[nodiscard]] std::optional<HeroScreen> findHeroScreen(const scene::SceneDescription& description,
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
    /// video.json (I5 step 1, ADR-0016 §4): clips at all, and each room's hero settings (its playlist, previewDuration,
    /// showOnFocus, the explicit games).
    VideoSettings video;
    /// The asset root: the title cards' bundled fonts.
    std::filesystem::path assetRoot;
};

/// The hero screen's play settings in room `roomId` (video.json's hero settings and the room's, the frozen runs').
[[nodiscard]] ScreenPlaySettings heroPlaySettings(const ScreenServiceOptions& options, std::string_view roomId);

/// The hero CRT's screen service (I3 step 11, ADR-0012 §7): one media::VideoDecoder at a time (a runtime assertion:
/// never two, their threads included, which the run report's maxDecoderThreads shows), driven by a ScreenPolicy (what
/// to play: the dwell, the start choice, the fallback chain; the launch's life), its frames uploaded into one
/// gl::VideoTexture (kept for the run, the CPU section videoUpload around each upload), the still pictures of the chain
/// (screenshot, title screen, cover) decoded and fitted to the screen's 4:3 on a worker thread and uploaded as hero
/// textures, and whichever is shown set as the texture override of the screen's material (gl::MaterialRegistry::
/// setTextureOverride; the idle pattern is the material's own colour bars). The decoder pauses while the screen is out
/// of the view; before a launch it is stopped and its thread waited for, at most kScreenStopBoundSeconds (a thread that
/// does not end in time is logged and the launch goes on), and after the return it opens again at the clip's position;
/// once Retroverse quits it never starts again, and shutdown() waits for every decoder thread
/// (media::waitForStoppedDecoders()). A clip that replaces another opens once the other's thread ended (the screen
/// shows the new game's still meanwhile), so that no two decoder threads ever run. Files are logged by game, never by
/// path (the user's folders). Main thread only; the material registry must outlive the service.
///
/// Since I5 step 1 (ADR-0016 §3, §8) it plays its room's playlist (ScreenPolicy::tick, the room's video.json
/// settings), ends the chain at the game's generated title card (assets::drawTitleCard on the worker, 640 x 480), and
/// keeps the picture shown up while the next game's is not ready (its still or its first frame): a switch never shows
/// the screen's own pattern, which only a screen without a game shows (the test room's colour bars, the building's
/// standby). The played games, the pictures shown and the frames held are recorded (VideoRecord).
///
/// Since I4 step 6b (ADR-0013 §9) the service follows the CRT of the room that holds the eye: bind() moves it to
/// another screen with that room's games, the clip of the previous one stopping as a clip replacement does (its
/// decoder's thread ends on its own and is never waited for on the render loop; the next clip opens once it has ended),
/// the previous screen's picture override cleared (its colour bars back). Any scene shown in place of another (a floor
/// committed again, the shell or the test room after a floor) lets go of its screen first (unbind(), the scene's
/// registry being cleared) and binds the new scene's CRT if it has one: one service, one decoder, for the run, and
/// shutdown() only at its end.
class ScreenService {
public:
    /// Rows the video is decoded at, at most (ADR-0012 §7: one 480p decoder).
    static constexpr int kMaxVideoRows = 480;
    /// The still pictures' size: the screen's 4:3, as the clips.
    static constexpr glm::ivec2 kStillSize{640, 480};

    ScreenService(HeroScreen screen, gl::MaterialRegistry& materials, std::vector<ScreenGame> games,
                  ScreenServiceOptions options);
    ~ScreenService();
    ScreenService(const ScreenService&) = delete;
    ScreenService& operator=(const ScreenService&) = delete;

    /// The games changed (ScreenPolicy::setGames).
    void setGames(std::vector<ScreenGame> games);

    /// Lets go of the screen (I4 step 6b): the clip stops as a clip replacement does (never waited for), the still
    /// goes, and the screen's picture override clears when `clearOverride` (false: the scene's registry was cleared,
    /// the material is gone). Unbound, the service shows nothing and opens no decoder until bind().
    void unbind(bool clearOverride);
    /// Binds `screen` (another room's CRT, or the same room's in a scene committed again) with `games`, the start
    /// choice made as at the start; recorded with `frame` (VideoRecord::binds).
    void bind(HeroScreen screen, std::vector<ScreenGame> games, std::uint64_t frame);
    /// A screen is bound, and its object id ("" none).
    [[nodiscard]] bool bound() const noexcept;
    [[nodiscard]] std::string screenId() const;

    /// One frame at `nowSeconds` (the run's simulated time) seen by `camera`: the item focused or inspected
    /// (`hovered`, its catalog id), the decoder opened, paused, followed and its frames uploaded, the stills loaded,
    /// the screen's texture override set to what is shown.
    void update(double nowSeconds, const scene::Camera& camera, std::optional<std::string_view> hovered);

    /// A launch starts (before the window's hand-over): the decoder stops and its thread is waited for (bounded).
    void launchStarting();
    /// The launch ended, failed to start or was refused: the decoder opens again at its position.
    void launchEnded();
    /// The end of the run: the decoder stays stopped, every decoder thread is waited for, the worker ends.
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
