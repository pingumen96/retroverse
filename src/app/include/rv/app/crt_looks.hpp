#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// The screens' CRT looks (I5 step 6, ADR-0016 §7): which of the presets of assets/materials/crt_presets.json
// (assets::CrtPreset) each screen of the world takes, decided when its material is registered. The first that applies:
//   1. the user's crtEffect (video.json): "off" makes every screen neutral; "vintage" steps the preset rules 2 to 4
//      chose one stronger (strongerCrtPreset(): monitor to consumer, consumer to arcade; arcade stays, and so does
//      neutral, the clean video); "light" (the default) keeps it;
//   2. video.json's crtPreset for the screen: its screen's ("rooms.<room>.screens.<object id>"), else its room's
//      ("rooms.<room>.<kind>"), else its kind's ("hero", "secondary", "cabinet"); "" (the shipped value) is none;
//   3. the room's theme pack's screenPreset (world::ThemeMaterials: the optical-era lounge's "monitor", the arcade's
//      "arcade");
//   4. the screen kind's default (defaultCrtPreset(): consumer, the cabinets arcade).
// A preset is a look: it moves nothing, and it changes only the CRT values of the screen's material block (the same
// shader path, the same cost). The screens: a scene's objects whose "screen" part draws with a CRT material (a
// "crt" block: MAT_Screen_CRT, MAT_Screen_CRT_Room), the hero ("...fx:hero.crt", built from the crt prefab or the
// arcade's hero cabinet; a hand-made scene's first crt), the secondaries (world::kSecondaryTvKind, a hand-made
// scene's further crts), and the arcade's cabinets (their collection's screen material, MAT_Screen_Arcade, one look
// for the room's cabinets: its room's cabinet kind's).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/app/scene_content_plan.hpp>
#include <rv/app/video_settings.hpp>
#include <rv/assets/crt_presets.hpp>
#include <rv/assets/material_desc.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

/// The id's end of a generated room's hero CRT, the hero corner's "crt" part ("room:snes/fx:hero.crt"), which the
/// arcade's hero cabinet keeps.
inline constexpr std::string_view kHeroCrtIdSuffix = ".crt";

/// Which rule chose a screen's preset (ADR-0016 §7's order, the user's crtEffect over all three).
enum class CrtRule : std::uint8_t {
    Kind,
    Pack,
    VideoSettings,
};
/// "kind", "pack", "video.json".
[[nodiscard]] std::string_view toString(CrtRule rule) noexcept;

/// A screen's preset: the one it draws with, the one rules 2 to 4 chose before the user's crtEffect, and which rule
/// that was.
struct CrtChoice {
    assets::CrtPreset preset = assets::CrtPreset::Consumer;
    assets::CrtPreset chosen = assets::CrtPreset::Consumer;
    CrtRule rule = CrtRule::Kind;

    bool operator==(const CrtChoice&) const = default;
};

/// The preset a screen of `kind` takes when nothing else chooses: consumer for the hero and the secondaries, arcade
/// for the cabinets.
[[nodiscard]] assets::CrtPreset defaultCrtPreset(ScreenKind kind) noexcept;
/// "vintage"'s step: monitor to consumer, consumer to arcade; arcade and neutral stay.
[[nodiscard]] assets::CrtPreset strongerCrtPreset(assets::CrtPreset preset) noexcept;

/// ADR-0016 §7's choice for a screen of `kind`: `videoPreset` is video.json's crtPreset for it (VideoSettings::screen()'s,
/// "" for none), `packPreset` its room's pack's screenPreset ("" for none); a name that is no preset counts as none.
[[nodiscard]] CrtChoice chooseCrtPreset(CrtEffect effect, std::string_view videoPreset, std::string_view packPreset,
                                        ScreenKind kind) noexcept;

/// The room a scene object stands in, as the screens' service names it: a floor's object ids start with their room's
/// ("room:snes/fx:hero.crt": "room:snes"), a single room's or a hand-made scene's object ids have none (its scene's
/// name: "m0_testroom", "screens_four").
[[nodiscard]] std::string screenRoomOf(std::string_view objectId, std::string_view sceneName);

/// Whether `object` holds the hero's role by its id (I5 step 3): a `crt`, or the arcade's hero cabinet that stands for
/// it, whose id ends with kHeroCrtIdSuffix.
[[nodiscard]] bool holdsHeroRole(const scene::SceneObject& object) noexcept;
/// Whether `object`'s screen can show pictures, findScreens()'s test before its material's: a prefab whose "screen"
/// role has a scene texture (so that its screen's material instance has a base colour texture to override).
[[nodiscard]] bool screenShowsPictures(const scene::SceneObject& object) noexcept;
/// The hero among `objects`, as findScreens() binds it: the first object holding the hero's role (holdsHeroRole())
/// whose screen `canBind`, else the first `crt` whose screen `canBind` (a hand-made scene's); none without either.
/// `canBind` (an object's index) is asked only of a role's holder and of the `crt`s before the first that binds: an
/// arcade's hundreds of cabinets are never asked.
[[nodiscard]] std::optional<std::size_t> heroScreenIndex(std::span<const scene::SceneObject> objects,
                                                         const std::function<bool(std::size_t)>& canBind);

/// The kind of `object` as a screen (findScreens()'s roles): the hero when it is its room's (`hero`: heroScreenIndex()'s);
/// else a secondary for world::kSecondaryTvKind and every other `crt`; none for any other object.
[[nodiscard]] std::optional<ScreenKind> screenKindOf(const scene::SceneObject& object, bool hero);

/// A screen's look as it was registered, for the run report (CrtLooksRecord).
struct ScreenLookApplied {
    /// The object's id, or "collection/<room id>/<family>" for a room's cabinets (their collection's key prefix: their
    /// static fronts and hero front), ".../video" for their pooled video materials once made.
    std::string screen;
    std::string room;
    ScreenKind kind = ScreenKind::Hero;
    CrtChoice choice;

    bool operator==(const ScreenLookApplied&) const = default;
};

/// The presets of a run and what chooses among them: video.json (its crtEffect and crtPreset keys) and the theme packs
/// of the rooms of the scene shown (their screenPreset, by room id), and the looks registered for that scene.
class ScreenLooks {
public:
    ScreenLooks() = default;
    /// `source`: where `presets` come from (assets::LoadedCrtPresets::source).
    ScreenLooks(assets::CrtPresets presets, std::string source) : m_presets(presets), m_source(std::move(source)) {}

    [[nodiscard]] const assets::CrtPresets& presets() const noexcept { return m_presets; }
    [[nodiscard]] const std::string& presetsSource() const noexcept { return m_source; }
    [[nodiscard]] const VideoSettings& video() const noexcept { return m_video; }
    void setVideo(VideoSettings video) { m_video = std::move(video); }
    /// The packs' screenPreset of the scene about to be shown, by room id (rooms without one left out); the looks
    /// recorded so far are dropped (the scene shown before is gone).
    void setScene(std::map<std::string, std::string, std::less<>> packPresets);
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& packPresets() const noexcept {
        return m_packPresets;
    }

    /// The choice for screen `screenId` of `kind` in room `roomId`, its pack's preset `packPreset` (none: the scene's,
    /// setScene()).
    [[nodiscard]] CrtChoice choose(ScreenKind kind, std::string_view roomId, std::string_view screenId,
                                   std::optional<std::string_view> packPreset = std::nullopt) const;
    /// The values of `choice`'s preset.
    [[nodiscard]] const assets::CrtEffect& effect(const CrtChoice& choice) const noexcept {
        return m_presets[choice.preset];
    }

    /// Records a screen's look (one per screen id: a screen registered again replaces its record).
    void record(ScreenLookApplied applied);
    /// Drops the looks recorded for the screens whose id starts with `prefix` ("room:snes/": a room's objects about to
    /// be rebuilt, SceneInstance::replaceObjects()); its cabinets' record ("collection/...") stays: their collection
    /// is laid out again (CollectionInstance::applyLayout()), never made again, and keeps its look.
    void forget(std::string_view prefix);
    /// The looks of the scene shown, by screen id.
    [[nodiscard]] std::vector<ScreenLookApplied> applied() const;

private:
    assets::CrtPresets m_presets = assets::builtInCrtPresets();
    std::string m_source = "built-in";
    VideoSettings m_video;
    std::map<std::string, std::string, std::less<>> m_packPresets;
    std::map<std::string, ScreenLookApplied, std::less<>> m_applied;
};

/// Gives the screens among `plan`'s materials their looks: each material instance of a screen (key "<object id>/screen",
/// optionally "@<w>x<h>") whose description has a CRT effect (MaterialDesc::crt) takes the effect of its object's
/// choice (`objects` are the scene's objects the plan was made from, in their order: SceneGeometry's; `sceneName` the
/// scene's name), recorded in `looks`. Each room's hero (screenRoomOf()) is heroScreenIndex()'s among its objects
/// whose screen shows pictures (screenShowsPictures()) through such an instance with a picture: the screen the
/// screens' service binds as the hero. Other materials are left as they are. Returns how many it changed.
std::size_t applyScreenLooks(SceneContentPlan& plan, std::span<const scene::SceneObject> objects,
                             std::string_view sceneName, ScreenLooks& looks);

} // namespace rv::app
