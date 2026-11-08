#pragma once

// The user's console models in the rooms (I11 step 7, ADR-0019 §7), GPU-free: which objects of a scene are a console
// room's hero console, the model object that takes its place, and its media socket for the insertion. The scene files
// and the plans keep the procedural console: the app swaps the model in at runtime (WorldConsoles, through
// SceneInstance::replaceObjects() under the console's own id), only once its model loaded and passed
// assets::checkConsoleModel(), so a world without content, or with a model that fails, draws its procedural console as
// before (never the magenta cube).
//
// The model stands where the procedural console stands: on the TV cabinet beside the CRT (console_room_9x10's console
// part, 0.42 m right of the hero corner's middle, turned 352 degrees), its bottom on the cabinet, its front +Z. A model
// wider than the cabinet leaves to the console's right (kConsoleRightReach on each side of its pivot) moves left, within
// the free width the footprint check allows.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include <rv/app/world_inspect.hpp>
#include <rv/assets/console_content.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/ids.hpp>

namespace rv::app {

/// Scene models of the user's content: "content:consoles/<system>/<model file>".
inline constexpr std::string_view kContentModelPrefix = "content:";
/// The hero console's local id in a console room.
inline constexpr std::string_view kHeroConsoleId = "fx:hero.console";
/// From the console's pivot to the cabinet's right edge (console_room_9x10: the cabinet's half width 0.6 m, the
/// console's part at 0.42 m).
inline constexpr float kConsoleRightReach = 0.18F;

/// A console room's procedural hero console in the scene: its room, its object and the object's transform.
struct ConsoleSite {
    world::RoomId room;
    std::string objectId;
    glm::mat4 transform{1.0F};

    bool operator==(const ConsoleSite&) const = default;
};

/// The procedural hero consoles of `description` ("<room>/fx:hero.console", a prefab, in a console room), in order.
[[nodiscard]] std::vector<ConsoleSite> consoleSites(const scene::SceneDescription& description);

/// "content:consoles/<system>/<model>", and the system such a path names (none for another path).
[[nodiscard]] std::string contentModelPath(std::string_view system, std::string_view modelFile);
[[nodiscard]] std::optional<std::string> contentModelSystem(std::string_view modelPath);

/// From the model's root to the console's site: assets::consolePlacement() (its front +Z, its bottom's middle at the
/// pivot), moved left when `width` (the placed model's, metres) reaches past the cabinet's right edge.
[[nodiscard]] glm::mat4 consoleModelLocal(const assets::ModelData& model, const assets::ConsoleManifest& manifest,
                                          float width) noexcept;

/// The model object in place of `site`'s console: the same id, a glTF model at `modelPath`, at the site's transform
/// times `local`; it casts shadows and collides (its box from the model's bounds).
[[nodiscard]] scene::SceneObject consoleModelObject(const ConsoleSite& site, std::string modelPath,
                                                    const glm::mat4& local);

/// The model's media socket in the scene (assets::consoleMediaSocket() under the site and `local`'s shift), with
/// `heroScreen` kept from the room's socket before.
[[nodiscard]] ConsoleSocket consoleModelSocket(const assets::ModelData& model, const assets::ConsoleManifest& manifest,
                                               const ConsoleSite& site, float width,
                                               std::optional<glm::mat4> heroScreen);

} // namespace rv::app
