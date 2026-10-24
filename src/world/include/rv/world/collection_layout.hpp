#pragma once

// The collection's layout (ADR-0010 §6, ADR-0011 §1-§3, ADR-0012 §2; I3 step 10a): one item per placement of a room,
// where it stands, how the renderer draws it and how the player reaches it. GPU-free: transforms, ids and boxes; the
// app (step 10b's app::CollectionInstance on step 8's static instance groups) draws what this describes, step 12 fills
// the layers, step 13a hovers and focuses through the pick targets and the navigation.
//
// - Stance: a placement in a face-out level (its slot volume the media family's item as it is) stands face-out, in a
//   spine-out one (the item on its end) spine-out; the plan's section must agree (featured and band face-out, spine
//   spine-out). The slot comes from the fixture's prefab ("Slot_<level>_<index>").
// - Transform (I3 step 1b's note): fixture * slot socket * the placement's offset * geometry::slotItemTransform(stance,
//   item size), from the item prefab's space (the cartridge, pivot centre) to the room's frame (meters); a spine-out
//   item of a family whose spineFacing is "side" stands on its side (geometry::SlotFacing::SideOut, I6 step 4). The
//   offset (ADR-0010 §6, stored in the plan): spine-out, pushed in along the slot's -Z by insetMm; face-out, turned by
//   yawCdeg about the slot's +Y (through the item's centre: the slot's origin is the middle of its bottom).
// - Pick id (ADR-0012 §2, step 7's PickId): the slot's, stable for as long as the template's version: (index of the
//   fixture's socket in the template's sockets + 1) << 24 | level << 14 | slot index; never 0. An item keeps its pick
//   id while its placement stands (step 9 adds placements, never moves them), whatever else the plan gains.
// - Pick target: the item's slot frame with its offset and the slot volume with its visible face inflated
//   (faceTargetBox); its cluster the item's.
// - Clusters (ADR-0011 §3): a level of a module (a shelf), the whole of any other fixture (a display case, the wall
//   shelf); id (socket index + 1) << 10 | level, level 1023 for a whole fixture; bounds the union of their items'
//   boxes in the room.
// - Groups: the items drawn with one media family's meshes (one in I3), their clusters and items contiguous: items in
//   (group, cluster, slot) order.
// - Instance record (step 0c): layers 0 (the pending picture: step 12 assigns the layers, keyed by frontLayerKey and
//   spineLayerKey), the flags (kUnavailable for a game the snapshot lacks or whose file is not available; the record's
//   also kNoSpineLabel for a face-out item, I6 step 4), the pick id.
// - Occluders (step 7: what hides an item behind it; not the walking colliders, not glass): the room's walls (the
//   shell's colliders), the solid pieces of every fixture with slots (a shelf's or the wall shelf's sides, back, base,
//   top and the boards between its levels: its prefab's bounds outside the levels' free volumes, the front left open;
//   a display case's base and back: its front, sides and top are glass), the bounds of every other fixture but the rug
//   (furniture, posters, lamps) and of every dressing prop; boxes in the room's frame.
// - Navigation (step 7's SlotNavigation): one run per wall (the fixtures with slots of the template's sockets on that
//   wall, left to right as one faces it) and one per fixture standing free (a display case); each fixture's levels
//   bottom to top by their slots' centre height, each level's slots left to right by their centre along the run's right
//   axis; slots without an item (empty or dressed) as kNoPick.
// A placement whose slot the built prefab lacks, or whose slot holds no item of the family in the plan's section's
// stance (a fallback prefab with other levels), becomes no item and is reported (CollectionLayout::skipped).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/room_entry.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/slot_navigation.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// What the room shows of a placed game besides its slot: the catalog's title (the spine's text, the hover's label)
/// and whether its file is there.
struct CollectionTitle {
    catalog::GameId game;
    std::string title;
    bool available = true;

    bool operator==(const CollectionTitle&) const = default;
};

/// The titles of a room's snapshot: every entry, by GameId; available when its availability is Available.
[[nodiscard]] std::vector<CollectionTitle> collectionTitles(const catalog::RoomSnapshot& snapshot);

/// How an item stands in its slot (geometry::SlotFacing).
enum class ItemStance : std::uint8_t {
    FaceOut,
    SpineOut,
};

/// "face-out", "spine-out".
[[nodiscard]] constexpr std::string_view toString(ItemStance stance) noexcept {
    return stance == ItemStance::FaceOut ? "face-out" : "spine-out";
}

/// A whole fixture's cluster level (see the file comment).
inline constexpr std::uint32_t kWholeFixtureLevel = 1023;

/// The pick id of `slot` in a room of `roomTemplate` (see the file comment). Errors: NotFound for a socket the template
/// lacks; OutOfRange for a template of more than 254 sockets, a level above 1023 or an index above 16383.
[[nodiscard]] Expected<PickId> slotPickId(const RoomTemplate& roomTemplate, const SlotId& slot);

/// One placed game.
struct CollectionItem {
    PickId pickId = kNoPick;
    catalog::GameId game;
    SlotId slot;
    Section section = Section::Spine;
    ItemStance stance = ItemStance::SpineOut;
    /// CollectionCluster::id of its cluster.
    std::uint32_t cluster = 0;
    /// The slot's frame with the placement's offset, local to the room (meters; +Z out of the visible face, the origin
    /// the middle of the slot's bottom), and the slot volume in it: the item's box.
    glm::mat4 slotFrame{1.0f};
    core::Aabb slotBox;
    /// From the item prefab's space (pivot centre) to the room's frame.
    glm::mat4 transform{1.0f};
    /// scene::instance_flags.
    std::uint32_t flags = 0;
    /// The catalog's title ("" when the snapshot lacks the game).
    std::string title;
    /// The texture layers' keys (gl::CollectionArrays, step 6): "<GameId>/front" and "<GameId>/spine".
    std::string frontLayerKey;
    std::string spineLayerKey;

    /// The instance record's attributes: layers 0 (pending), the flags (with scene::instance_flags::kNoSpineLabel for a
    /// face-out item: its spine label is not drawn), the pick id.
    [[nodiscard]] scene::InstanceAttributes attributes() const noexcept;
    [[nodiscard]] scene::InstanceData instance() const noexcept;
    /// Its pick target (faceTargetBox of the slot box in the slot frame).
    [[nodiscard]] PickTarget pickTarget() const noexcept;
    /// Its box in the room's frame (the slot box through the slot frame).
    [[nodiscard]] core::Aabb worldBounds() const noexcept;
};

/// Items culled together (a level of a module, a display case).
struct CollectionCluster {
    std::uint32_t id = 0;
    FixtureId fixture;
    /// The level, or kWholeFixtureLevel.
    std::uint32_t level = kWholeFixtureLevel;
    /// The union of its items' boxes, in the room's frame.
    core::Aabb bounds;
    std::uint32_t firstItem = 0;
    std::uint32_t itemCount = 0;
};

/// The items drawn with one media family's shared meshes.
struct CollectionGroup {
    std::string mediaFamily;
    /// The family's item prefab and its size (meters: width, height, depth, the front facing +Z).
    PrefabRef item;
    glm::vec3 itemSize{0.0f};
    /// The material of the items' shells (MediaFamily::shellMaterial, I6 step 4's review); empty: the theme's
    /// "cartridge" role.
    std::string shellMaterial;
    std::uint32_t firstCluster = 0;
    std::uint32_t clusterCount = 0;
    std::uint32_t firstItem = 0;
    std::uint32_t itemCount = 0;
};

/// A placement that became no item, and why.
struct SkippedPlacement {
    Placement placement;
    std::string reason;
};

struct CollectionLayout {
    RoomId room;
    /// In (group, cluster, slot) order.
    std::vector<CollectionItem> items;
    /// By id, within their group.
    std::vector<CollectionCluster> clusters;
    std::vector<CollectionGroup> groups;
    /// The room's occluders (see the file comment), in the room's frame.
    std::vector<core::Aabb> occluders;
    /// Wall runs in the template's wall order (north, south, east, west), then the free-standing fixtures by id.
    std::vector<NavRun> navigation;
    std::vector<SkippedPlacement> skipped;

    /// The item of `id`, or nullptr.
    [[nodiscard]] const CollectionItem* find(PickId id) const noexcept;
    /// The item of `game`, or nullptr.
    [[nodiscard]] const CollectionItem* findGame(const catalog::GameId& game) const noexcept;
    /// Every item's pick target, in item order: what PickIndex::build takes with `occluders`.
    [[nodiscard]] std::vector<PickTarget> pickTargets() const;
};

/// The collection of `room` of `plan` (see the file comment), the titles giving each game's title and availability
/// (a game they lack: no title, unavailable). Errors: NotFound for a room the plan lacks, a fixture a placement or
/// dressed level names that the plan lacks, a prefab `data` lacks; ValidationFailed for two placements in one slot;
/// those of slotPickId().
[[nodiscard]] Expected<CollectionLayout> collectionLayout(const WorldPlan& plan, const RoomId& room,
                                                          const SceneData& data,
                                                          std::span<const CollectionTitle> titles);

/// The layout as a canonical document (core::writeCanonicalJson: integers only), for golden snapshots and their
/// digests: lengths in micrometres and matrix elements in millionths (column by column), each rounded to the nearest
/// integer; boxes as their minimum then maximum corner; navigation slots as [along, pick id]; items,
/// clusters, groups, occluders, navigation and skipped placements in their order.
[[nodiscard]] nlohmann::json collectionLayoutToJson(const CollectionLayout& layout);

} // namespace rv::world
