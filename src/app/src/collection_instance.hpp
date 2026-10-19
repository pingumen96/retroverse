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

namespace rv::app {

class SceneGraphics;

class CollectionInstance {
public:
    /// Registers the family's meshes and the collection's materials in `graphics`' registries (after the scene's: the
    /// materials are uploaded again), creates the arrays of `graphics`' format for the room's placements (rounded up to
    /// 64 layers), acquires every item's layers and uploads the group's records. Errors: the item prefab is missing
    /// or has no shell, a mesh or the arrays cannot be created, the group cannot be built.
    [[nodiscard]] static Expected<std::unique_ptr<CollectionInstance>> create(const world::CollectionLayout& layout,
                                                                              const world::SceneData& data,
                                                                              SceneGraphics& graphics,
                                                                              int uniformBufferOffsetAlignment);
    ~CollectionInstance();
    CollectionInstance(const CollectionInstance&) = delete;
    CollectionInstance& operator=(const CollectionInstance&) = delete;

    /// The view's static groups (RenderView::staticGroups).
    [[nodiscard]] std::span<const gl::StaticInstanceGroup* const> groups() const noexcept { return m_groupList; }
    [[nodiscard]] const gl::StaticInstanceGroup& group() const noexcept { return m_group; }
    [[nodiscard]] gl::CollectionArrays& arrays() noexcept { return m_arrays; }
    [[nodiscard]] const gl::CollectionArrays& arrays() const noexcept { return m_arrays; }
    [[nodiscard]] std::size_t itemCount() const noexcept { return m_items.size(); }

    /// The item of a layer key ("<GameId>/front", "<GameId>/spine"), or nothing.
    [[nodiscard]] std::optional<std::size_t> itemOfKey(const std::string& key) const;
    /// A layer went up: the record that shows it (the front's or the spine's) names it from now on.
    [[nodiscard]] Status layerResident(const gl::LayerUpload& upload);

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
    std::optional<scene::MeshHandle> m_frontMesh;
    /// The hero front's material and its own base colour texture (overridden by the hero texture shown).
    gl::Texture2D m_heroBase;
    scene::MaterialHandle m_heroMaterial;
    scene::ShaderKind m_heroShader = scene::ShaderKind::Pbr;
};

} // namespace rv::app
