#pragma once

// CollectionInstance (I3 step 10b; ADR-0011 §1 to §4): the room's collection on the GPU, beside the scene's objects.
// One entry per item of the collection's layout (world::CollectionLayout, I3 step 10a) with the media family's shared
// meshes (the item prefab's parts: the shell, the front label quad, the spine quad; one mesh each per family, never per
// game), its transform, its pick id, its texture layers and its flags, drawn as one static instance group (step 8:
// pre-sorted batches uploaded once, the clusters culled per view, the shadow map's too; shells cast, labels and spines
// do not). The fronts and spines sample the collection's texture arrays (step 6) through one layered material per array
// and class; every item holds a layer of each class from the start, its record naming layer 0 (the pending picture)
// until its picture is uploaded (layerResident(): one 16-byte write of the record that shows it). setHovered() flags an
// item's records highlighted (the hover outline, step 8).

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
    /// The game of the item of `pickId`, if any.
    [[nodiscard]] std::optional<catalog::GameId> gameOf(world::PickId pickId) const;

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
    };

    [[nodiscard]] Status writeFlags(std::size_t item, bool highlighted);
    [[nodiscard]] Status writeRecord(std::size_t source, const scene::InstanceAttributes& attributes);

    gl::CollectionArrays m_arrays;
    gl::StaticInstanceGroup m_group;
    std::array<const gl::StaticInstanceGroup*, 1> m_groupList{};
    std::vector<Item> m_items;
    /// Every record's attributes as written (the group's source order).
    std::vector<scene::InstanceAttributes> m_attributes;
    std::map<std::string, std::size_t, std::less<>> m_byKey;
    std::map<world::PickId, std::size_t> m_byPick;
    world::PickId m_hovered = world::kNoPick;
};

} // namespace rv::app
