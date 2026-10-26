#pragma once

// CollectionInstance (I3 step 10b; ADR-0011 §1 to §4): the room's collection on the GPU, beside the scene's objects.
// One entry per item of the collection's layout (world::CollectionLayout, I3 step 10a) with the media family's shared
// meshes (the item prefab's parts: the shell, the front label quad, the spine quad; one mesh each per family, never per
// game), its transform, its pick id, its texture layers and its flags, drawn as one static instance group (step 8:
// pre-sorted batches uploaded once, the clusters culled per view, the shadow map's too; shells cast, labels and spines
// do not). The fronts and spines sample the collection's texture arrays (step 6) through one layered material per array
// and class; every item holds a layer of each class from the start, its record naming layer 0 (the pending picture)
// until its picture is uploaded (layerResident(): one 16-byte write of the record that shows it). setHovered() flags an
// item's records highlighted (the hover outline, step 8); setHidden() flags them hidden while the item is drawn off its
// slot on the dynamic path (appendRenderItems(): its parts as render items at another pose, I3 step 13a).

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/render/collection_arrays.hpp>
#include <rv/render/static_instance_group.hpp>
#include <rv/render/texture.hpp>
#include <rv/scene/instance_clusters.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/scene_builder.hpp>

namespace rv::assets {
class MaterialLibrary;
} // namespace rv::assets

namespace rv::gl {
class MaterialRegistry;
} // namespace rv::gl

namespace rv::app {

class SceneGraphics;

/// What CollectionInstance::applyLayout did (I3 step 15b).
struct CollectionUpdate {
    /// Items whose layers were released (their games gone from the room).
    std::size_t released = 0;
    /// The arrays grew (a GPU copy), and an array was added (with its layered material).
    bool grown = false;
    bool arraysAdded = false;
};

/// How many arrays a collection holds and how many of them its layered materials sample (I4 step 5a: all of them, each
/// material its own room's array).
struct CollectionArraysSampled {
    std::size_t arrays = 0;
    std::size_t own = 0;
};

/// A collection's arrays taken over by the next collection of the same room (I3 step 15b: the room committed again
/// over the one shown keeps the pictures its items hold), with the family they were made for and the layer keys of the
/// items that held them.
struct CollectionPictures {
    gl::CollectionArrays arrays;
    world::RoomId room;
    std::string family;
    std::vector<std::string> frontKeys;
    std::vector<std::string> spineKeys;
};

class CollectionInstance {
public:
    /// Registers the family's meshes and the collection's materials in `graphics`' registries (after the scene's: the
    /// materials are uploaded again), creates the arrays of `graphics`' format for the room's placements (rounded up to
    /// 64 layers), acquires every item's layers and uploads the group's records. Errors: the item prefab is missing
    /// or has no shell, a mesh or the arrays cannot be created, the group cannot be built.
    ///
    /// `pictures` (I3 step 15b): the arrays of the collection this one replaces, used instead of new ones when they are
    /// of the same family and format: the layers of the games gone released, the arrays grown to the room, every item
    /// that stood showing its picture from the first frame.
    ///
    /// `keyPrefix` (I6 step 5): the registry keys' prefix in place of "collection/<room id>" (the hub's showcase, a
    /// collection of no room: hub_showcase.hpp's kShowcaseKeyPrefix).
    [[nodiscard]] static Expected<std::unique_ptr<CollectionInstance>>
    create(const world::CollectionLayout& layout, const world::SceneData& data, SceneGraphics& graphics,
           int uniformBufferOffsetAlignment, std::optional<CollectionPictures> pictures = std::nullopt,
           std::string_view keyPrefix = {});
    /// Gives up its arrays to the collection that replaces it (CollectionInstance::create's `pictures`); this one is
    /// only to be destroyed then.
    [[nodiscard]] CollectionPictures takePictures();
    ~CollectionInstance();
    CollectionInstance(const CollectionInstance&) = delete;
    CollectionInstance& operator=(const CollectionInstance&) = delete;

    /// The live update (I3 step 15b, world_live_plan.hpp's Items): `layout` (another collection of the same room, its
    /// static part unchanged, the same media family) applied in place. The items of the games that stood keep their
    /// layers and the pictures in them; the new ones acquire layers (the arrays grown by a GPU copy past their
    /// headroom, an array added with its layered material), the gone ones release theirs; the static group is made
    /// again from the layout's records (flags as the layout has them, the hovered item still highlighted when it
    /// stands). The meshes and materials stay. Call it at a frame boundary. Errors: another family (nothing changed),
    /// those of the arrays' growth and of the group's build (the collection may then be partly updated: commit the
    /// room again).
    [[nodiscard]] Expected<CollectionUpdate> applyLayout(const world::CollectionLayout& layout, SceneGraphics& graphics,
                                                         int uniformBufferOffsetAlignment);

    /// The view's static groups (RenderView::staticGroups).
    [[nodiscard]] std::span<const gl::StaticInstanceGroup* const> groups() const noexcept { return m_groupList; }
    [[nodiscard]] const gl::StaticInstanceGroup& group() const noexcept { return m_group; }
    [[nodiscard]] gl::CollectionArrays& arrays() noexcept { return m_arrays; }
    [[nodiscard]] const gl::CollectionArrays& arrays() const noexcept { return m_arrays; }
    [[nodiscard]] std::size_t itemCount() const noexcept { return m_items.size(); }
    /// The room it draws (its layout's).
    [[nodiscard]] const world::RoomId& room() const noexcept { return m_room; }
    /// Its arrays, and how many of them the layered materials registered for it sample (`materials`: the registry it
    /// was created in): the check that every room samples its own arrays (the run report's residency.rooms).
    [[nodiscard]] CollectionArraysSampled sampledArrays(const gl::MaterialRegistry& materials) const;

    /// The item of a layer key ("<GameId>/front", "<GameId>/spine"), or nothing.
    [[nodiscard]] std::optional<std::size_t> itemOfKey(const std::string& key) const;
    /// A layer went up: the record that shows it (the front's or the spine's) names it from now on.
    [[nodiscard]] Status layerResident(const gl::LayerUpload& upload);
    /// Layers that went up into this collection (layerResident()), and of them those over a layer its item's record
    /// named already: a picture uploaded again (I4 step 6b: none for a room a reconcile left unchanged).
    [[nodiscard]] std::uint64_t uploads() const noexcept { return m_uploads; }
    [[nodiscard]] std::uint64_t reuploads() const noexcept { return m_reuploads; }

    /// Highlights the item of `pickId` (kNoPick: none), its records flagged instance_flags::kHighlighted and the
    /// previous item's cleared.
    [[nodiscard]] Status setHovered(world::PickId pickId);
    [[nodiscard]] world::PickId hovered() const noexcept { return m_hovered; }
    /// Hides (or shows again) the static records of the item of `pickId` (instance_flags::kHidden: the vertex stage
    /// draws nothing for them, in every pass); nothing for an unknown item.
    [[nodiscard]] Status setHidden(world::PickId pickId, bool hidden);
    /// The items whose static records are drawn only in part or not at all (some record flagged hidden, but a front
    /// the hero texture shows in place, setFrontHidden()), ascending.
    [[nodiscard]] std::vector<world::PickId> hiddenItems() const;
    /// Whether some static record of the item of `pickId` is drawn (not flagged hidden); false for an unknown item.
    [[nodiscard]] bool staticDrawn(world::PickId pickId) const;
    /// The parts of the item of `pickId` as render items at `model` (the item prefab's space to the room, as the
    /// layout's transform), with their records' attributes as written (its layers, its highlight) but the hidden flag:
    /// the item drawn off its slot on the dynamic path; its front with the hero material while the hero texture shows it
    /// (setFrontHidden()). Appends nothing for an unknown item.
    void appendRenderItems(world::PickId pickId, const glm::mat4& model, std::vector<scene::RenderItem>& out) const;
    /// The game of the item of `pickId`, if any.
    [[nodiscard]] std::optional<catalog::GameId> gameOf(world::PickId pickId) const;
    /// Every item's box in the room, in the layout's order (the cover loader ranks by them).
    [[nodiscard]] std::vector<core::Aabb> itemBounds() const;
    /// The pick id of the item at `item` (its index in the layout's order), kNoPick past the end.
    [[nodiscard]] world::PickId pickOf(std::size_t item) const noexcept {
        return item < m_items.size() ? m_items[item].pickId : world::kNoPick;
    }
    /// The item of `pickId` (its index in the layout's order), if any.
    [[nodiscard]] std::optional<std::size_t> itemOfPick(world::PickId pickId) const;

    /// The hero texture's front (I3 step 12, ADR-0011 §8): the material that draws a hero texture on the family's front
    /// quad (its base colour overridden with the texture, MaterialRegistry::setTextureOverride), the item's front
    /// record hidden while the dynamic path draws it, and the render item that does (the front quad at the item's
    /// transform with `material`, drawn like the hero material, its attributes but kHidden, casting no shadow). Errors:
    /// no item, no front part.
    [[nodiscard]] scene::MaterialHandle heroMaterial() const noexcept { return m_heroMaterial; }
    [[nodiscard]] Status setFrontHidden(std::size_t item, bool hidden);
    [[nodiscard]] Expected<scene::RenderItem> frontRenderItem(std::size_t item, scene::MaterialHandle material) const;

private:
    CollectionInstance() = default;

    struct Item {
        world::PickId pickId = world::kNoPick;
        catalog::GameId game;
        std::string frontKey;
        std::string spineKey;
        /// Its records' source indices in the group, per part (shell, front, spine; nothing for a part the family's
        /// prefab lacks).
        std::optional<std::size_t> shell;
        std::optional<std::size_t> front;
        std::optional<std::size_t> spine;
        /// From the item prefab's space to the room (world::CollectionItem::transform), and its box in the room.
        glm::mat4 transform{1.0f};
        core::Aabb bounds;
        /// Off its slot (setHidden: every record hidden), and its front shown by the hero texture (setFrontHidden):
        /// the front record stays hidden while either holds.
        bool pulled = false;
        bool heroFront = false;
    };

    /// One layered material per array and class not yet given one.
    void addLayeredMaterials(SceneGraphics& graphics);
    /// The items of `layout`: their layers acquired, their records built (resident layers named; the hover's
    /// highlight and the pulled-out and hero-front hidden reasons of the items that stand carried over by pick id) and
    /// the group made.
    [[nodiscard]] Status setItems(const world::CollectionLayout& layout, SceneGraphics& graphics);
    [[nodiscard]] Status writeFlag(std::size_t item, std::uint32_t flag, bool on);
    /// Writes the hidden flag of the item's records from its `pulled` and `heroFront`.
    [[nodiscard]] Status writeHidden(std::size_t item);
    [[nodiscard]] Status writeRecord(std::size_t source, const scene::InstanceAttributes& attributes);

    gl::CollectionArrays m_arrays;
    gl::StaticInstanceGroup m_group;
    std::array<const gl::StaticInstanceGroup*, 1> m_groupList{};
    std::vector<Item> m_items;
    /// Every record's attributes as written, and the record as it was listed (the group's source order).
    std::vector<scene::InstanceAttributes> m_attributes;
    std::vector<scene::StaticInstance> m_sources;
    std::map<std::string, std::size_t, std::less<>> m_byKey;
    std::map<world::PickId, std::size_t> m_byPick;
    world::PickId m_hovered = world::kNoPick;
    std::uint64_t m_uploads = 0;
    std::uint64_t m_reuploads = 0;
    std::optional<scene::MeshHandle> m_frontMesh;
    /// The media family, its part meshes (shell, front, spine), the shell's material, the label and spine materials'
    /// names and one layered material per array and class (kept for the live update's new items and arrays).
    std::string m_family;
    /// Its room and its registry keys' prefix ("collection/<room id>/<family>", I4 step 5a).
    world::RoomId m_room;
    std::string m_keyPrefix;
    std::array<std::optional<scene::MeshHandle>, 3> m_partMeshes{};
    scene::MaterialHandle m_shellMaterial;
    std::array<std::string, gl::kCollectionClassCount> m_labelMaterials;
    std::array<std::vector<scene::MaterialHandle>, gl::kCollectionClassCount> m_layered;
    /// The hero front's material and its own base colour texture (overridden by the hero texture shown).
    gl::Texture2D m_heroBase;
    scene::MaterialHandle m_heroMaterial;
    scene::ShaderKind m_heroShader = scene::ShaderKind::Pbr;
};

} // namespace rv::app
