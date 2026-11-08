#pragma once

// The rooms' signs from the systems' identity (I11 step 4, ADR-0019 §3), GPU-free: which objects of a floor's scene are
// a room's signs, and what each shows. The scene files and the plans keep their generic signs ("16-BIT CORNER", the
// bitmap lettering): the app draws the identity's sign (assets::makeIdentitySign()) and puts it on the sign's material
// at runtime (WorldSigns), as the hero CRT's pictures are put on its screen. A room whose systems have no identity keeps
// its generic sign, so every scene without a theme draws as before.
//
// The signs (floor_scene.cpp, scene_builder.cpp), each with its "picture" a "neon" or "sign" pattern:
//   - "<room>/fx:hero.sign" (and a pack's further pieces, ".2"...): the room's neon over its hero corner;
//   - "hall:<floor>/portal:<room>/sign", ".../blade-east", ".../blade-west" (also under the hub's "hub/"): the plate over
//     a room's doorway and the blades beside it;
//   - "hall:<floor>/hub/fascia-sign:<room>": the hub's fascia over a room's doorway.

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/identity_sign.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/ids.hpp>

namespace rv::app {

/// The role of a sign's picture.
inline constexpr std::string_view kSignPictureRole = "picture";

/// Where a sign stands.
enum class IdentitySignPlace : std::uint8_t {
    /// The room's own neon, over its hero corner.
    Hero,
    /// A portal's plate, a blade beside it, the hub's fascia: the hall's.
    Hall,
};

/// A sign of the scene: its object, the room it names, where it stands, and its style (its pattern's: "neon" draws the
/// neon, "sign" the lit plate).
struct IdentitySignTarget {
    std::string objectId;
    world::RoomId room;
    IdentitySignPlace place = IdentitySignPlace::Hall;
    assets::IdentitySignStyle style = assets::IdentitySignStyle::Lit;

    bool operator==(const IdentitySignTarget&) const = default;
};

/// The signs of `description`, in its objects' order: the objects above whose "picture" is a "neon" or "sign" pattern
/// and whose room id parses. Anything else (the floor's own sign, the entrance's, the totem) is not a room's.
[[nodiscard]] std::vector<IdentitySignTarget> identitySignTargets(const scene::SceneDescription& description);

/// The room a sign object's id names, and where it stands (identitySignTargets()'s rules on the id alone).
[[nodiscard]] std::optional<std::pair<world::RoomId, IdentitySignPlace>> identitySignRoom(std::string_view objectId);

/// A system as its sign shows it (from the catalog: CatalogSystem's fullName, esdeName and logo).
struct SignSystem {
    /// The name people read (its identity's, else ES-DE's <fullname>).
    std::string name;
    /// ES-DE's <fullname>.
    std::string esdeName;
    /// Its logo file, empty without one.
    std::filesystem::path logo;
};

/// A system has an identity to show: a logo file, or a name its identity gives (one that differs from ES-DE's).
[[nodiscard]] bool hasIdentity(const SignSystem& system) noexcept;

/// What a sign shows: its logos (files, in order, at most assets::kMaxSignLogos) and its name (UTF-8, possibly empty).
struct IdentitySignContent {
    std::vector<std::filesystem::path> logos;
    std::string name;

    bool operator==(const IdentitySignContent&) const = default;
};

using SignSystemLookup = std::function<std::optional<SignSystem>(const catalog::SystemName&)>;

/// What the sign of `room` at `place` shows, none to keep its generic sign:
///   - a console room, when its system has an identity: its logo alone (a theme's logo is a wordmark: it says the name
///     already), else its name; an expansion's number after either ("Lumen 16 2", as chainSignText() numbers the
///     generic one);
///   - a shared or arcade room (`sections`, the plan's): on the hall's signs, the logos of its sections that have one, in
///     order, with an expansion's number as the name; its own neon keeps its generic sign (it names the room's kind).
[[nodiscard]] std::optional<IdentitySignContent> identitySignContent(const world::RoomId& room,
                                                                     IdentitySignPlace place,
                                                                     std::span<const catalog::SystemName> sections,
                                                                     const SignSystemLookup& lookup);

} // namespace rv::app
