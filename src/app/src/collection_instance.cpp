#include "collection_instance.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/scene_graphics.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/scene/instance_clusters.hpp>

namespace rv::app {
namespace {

/// The parts of a media family's item, in the order each item's records are listed (its source indices).
enum class Part : std::uint8_t { Shell, Front, Spine };
constexpr std::array<std::string_view, 3> kPartRoles{"plastic_shell", "label", "spine"};

/// The pending picture of both classes: a neutral label in the family's plastic colour (ADR-0011 §6), from the shell
/// material's base colour (a dark case's dark, I6 step 4's review).
[[nodiscard]] gl::Rgb8 pendingColorOf(const assets::MaterialDesc& shell) {
    const assets::Rgb8 srgb =
        assets::toSrgb8(assets::LinearRgb{shell.baseColorFactor.r, shell.baseColorFactor.g, shell.baseColorFactor.b});
    return gl::Rgb8{srgb.r, srgb.g, srgb.b};
}

} // namespace

CollectionInstance::~CollectionInstance() = default;

Expected<std::unique_ptr<CollectionInstance>> CollectionInstance::create(const world::CollectionLayout& layout,
                                                                         const world::SceneData& data,
                                                                         SceneGraphics& graphics,
                                                                         int uniformBufferOffsetAlignment,
                                                                         std::optional<CollectionPictures> pictures) {
    std::unique_ptr<CollectionInstance> instance(new CollectionInstance());
    if (layout.groups.size() != 1) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("{} media families in the collection: I3 draws one", layout.groups.size()),
                         layout.room.toString());
    }
    const world::CollectionGroup& family = layout.groups.front();
    const world::ScenePrefab* item = data.prefabs.find(family.item);
    if (item == nullptr || !item->prefab) {
        return makeError(ErrorCode::NotFound, "no procedural prefab for the collection's items", family.item.manifest);
    }
    const geometry::ProceduralPrefab& prefab = *item->prefab;

    // The family's meshes, one per part, shared by every item of the room. Every registry key names the room (I4 step
    // 5a, ADR-0013 §9): two rooms of one family on a floor never share a material, so each samples its own arrays.
    gl::MeshRegistry& meshes = graphics.meshes();
    instance->m_family = family.mediaFamily;
    instance->m_room = layout.room;
    instance->m_keyPrefix = fmt::format("collection/{}/{}", layout.room.toString(), family.mediaFamily);
    auto& partMeshes = instance->m_partMeshes;
    for (std::size_t part = 0; part < kPartRoles.size(); ++part) {
        const geometry::PrefabPart* found = geometry::findPart(prefab, kPartRoles[part]);
        if (found == nullptr) {
            continue;
        }
        auto handle = meshes.add(fmt::format("{}/{}", instance->m_keyPrefix, kPartRoles[part]), found->mesh);
        if (!handle) {
            return propagate(handle.error());
        }
        partMeshes[part] = *handle;
    }
    if (!partMeshes[static_cast<std::size_t>(Part::Shell)]) {
        return makeError(ErrorCode::ValidationFailed, "the collection's item prefab has no shell",
                         family.item.manifest);
    }

    // The materials: the family's shell (I6 step 4's review: a case's or box's own, MediaFamily::shellMaterial), else the
    // theme's, and one layered material per array and class.
    const std::map<std::string, std::string> roles = world::themeMaterials("cartridge");
    const auto roleMaterial = [&](std::string_view role) {
        const auto found = roles.find(std::string(role));
        return found != roles.end() ? found->second : std::string("MAT_Label_Print");
    };
    const assets::MaterialLibrary& library = graphics.materialLibrary();
    gl::MaterialRegistry& materials = graphics.materials();
    const assets::MaterialDesc& shellDesc = library.getOrFallback(
        family.shellMaterial.empty() ? roleMaterial("plastic_shell") : family.shellMaterial);
    instance->m_shellMaterial =
        materials.add(fmt::format("{}/shell", instance->m_keyPrefix), "collection shell", shellDesc,
                      gl::resolveMaterialTextures(shellDesc, graphics.textures()));
    instance->m_labelMaterials = {roleMaterial("label"), roleMaterial("spine")};

    gl::CollectionArrays::Desc desc;
    desc.format = graphics.collectionArrayFormat();
    const gl::Rgb8 pending = pendingColorOf(shellDesc);
    desc.pendingColors = {pending, pending};
    desc.debugName = "Collection " + layout.room.toString();
    // Sized to the placements (planArrayLayers rounds them up to 64): 2,000 items take one array of 2,048 layers per
    // class, within ADR-0011 §9's 80 MiB; a placement added later takes a layer of that headroom or grows the arrays by
    // a GPU copy (CollectionArrays::reserve, step 15b).
    const std::size_t slots = layout.items.size();
    if (pictures && pictures->room == layout.room && pictures->family == family.mediaFamily &&
        pictures->arrays.format() == desc.format) {
        // I3 step 15b: the arrays of the collection shown before, the layers of the games gone released.
        std::set<std::string, std::less<>> keys;
        for (const world::CollectionItem& placed : layout.items) {
            keys.insert(placed.frontLayerKey);
            keys.insert(placed.spineLayerKey);
        }
        for (const std::string& key : pictures->frontKeys) {
            if (!keys.contains(key)) {
                pictures->arrays.release(gl::CollectionClass::Front, key);
            }
        }
        for (const std::string& key : pictures->spineKeys) {
            if (!keys.contains(key)) {
                pictures->arrays.release(gl::CollectionClass::Spine, key);
            }
        }
        for (const gl::CollectionClass cls : gl::kCollectionClasses) {
            if (Status reserved = pictures->arrays.reserve(cls, slots); !reserved) {
                return propagate(reserved.error());
            }
        }
        instance->m_arrays = std::move(pictures->arrays);
    } else {
        auto arrays = gl::CollectionArrays::create(desc, {slots, slots});
        if (!arrays) {
            return propagate(arrays.error());
        }
        instance->m_arrays = std::move(*arrays);
    }

    instance->addLayeredMaterials(graphics);
    // The hero textures' front (I3 step 12): the label's material with a base colour texture of its own (white), which
    // the hero texture shown overrides.
    {
        const assets::MaterialDesc& labelDesc = library.getOrFallback(roleMaterial("label"));
        // Clamped like a picture: the sampler bound with the override follows this texture's wrap mode.
        assets::ImageData white;
        white.width = 1;
        white.height = 1;
        white.channels = 4;
        white.pixels = {255, 255, 255, 255};
        gl::TextureDesc whiteDesc;
        whiteDesc.mipmaps = false;
        whiteDesc.sampling.wrap = gl::TextureWrap::ClampToEdge;
        whiteDesc.memoryKind = gl::GpuMemoryKind::HeroTextures;
        auto base = gl::Texture2D::create(white, whiteDesc, "Collection hero base");
        if (!base) {
            return propagate(base.error());
        }
        instance->m_heroBase = std::move(*base);
        gl::MaterialTextures textures = gl::resolveMaterialTextures(labelDesc, graphics.textures());
        textures[static_cast<std::size_t>(gl::MaterialTextureSlot::BaseColor)] = &instance->m_heroBase;
        instance->m_heroMaterial = materials.add(fmt::format("{}/hero", instance->m_keyPrefix),
                                                 "collection hero front", labelDesc, textures);
        instance->m_heroShader = materials.material(instance->m_heroMaterial).shader;
    }
    instance->m_frontMesh = partMeshes[static_cast<std::size_t>(Part::Front)];
    if (Status uploaded = materials.upload(uniformBufferOffsetAlignment, "MaterialUniforms"); !uploaded) {
        return propagate(uploaded.error());
    }

    if (Status items = instance->setItems(layout, graphics); !items) {
        return propagate(items.error());
    }
    spdlog::info(
        "Collection of {}: {} item(s) in {} cluster(s), {} record(s) in {} batch(es) ({} bytes); arrays {} for "
        "{} item(s) ({:.1f} MiB)",
        layout.room.toString(), layout.items.size(), layout.clusters.size(), instance->m_attributes.size(),
        instance->m_group.layout().batches().size(), instance->m_group.uploadedBytes(), gl::toString(desc.format),
        slots, static_cast<double>(instance->m_arrays.estimatedBytes()) / (1024.0 * 1024.0));
    return instance;
}

CollectionArraysSampled CollectionInstance::sampledArrays(const gl::MaterialRegistry& materials) const {
    CollectionArraysSampled sampled;
    for (const gl::CollectionClass cls : gl::kCollectionClasses) {
        const std::size_t count = m_arrays.arrayCount(cls);
        sampled.arrays += count;
        for (const scene::MaterialHandle handle : m_layered[gl::toIndex(cls)]) {
            const gl::Texture2DArray* array = materials.material(handle).layeredBaseColor;
            for (std::size_t index = 0; index < count; ++index) {
                if (array == &m_arrays.array(cls, index)) {
                    ++sampled.own;
                    break;
                }
            }
        }
    }
    return sampled;
}

CollectionPictures CollectionInstance::takePictures() {
    CollectionPictures pictures;
    pictures.room = m_room;
    pictures.family = m_family;
    for (const Item& item : m_items) {
        pictures.frontKeys.push_back(item.frontKey);
        pictures.spineKeys.push_back(item.spineKey);
    }
    pictures.arrays = std::move(m_arrays);
    return pictures;
}

void CollectionInstance::addLayeredMaterials(SceneGraphics& graphics) {
    const assets::MaterialLibrary& library = graphics.materialLibrary();
    gl::MaterialRegistry& materials = graphics.materials();
    for (const gl::CollectionClass cls : gl::kCollectionClasses) {
        const bool spine = cls == gl::CollectionClass::Spine;
        const assets::MaterialDesc& labelDesc = library.getOrFallback(m_labelMaterials[gl::toIndex(cls)]);
        std::vector<scene::MaterialHandle>& layered = m_layered[gl::toIndex(cls)];
        for (std::size_t index = layered.size(); index < m_arrays.arrayCount(cls); ++index) {
            const std::string key = fmt::format("{}/{}[{}]", m_keyPrefix, gl::toString(cls), index);
            layered.push_back(materials.addLayered(key, key, labelDesc,
                                                   gl::resolveMaterialTextures(labelDesc, graphics.textures()),
                                                   gl::LayeredBaseColor{&m_arrays.array(cls, index), spine}));
        }
    }
}

Status CollectionInstance::setItems(const world::CollectionLayout& layout, SceneGraphics& graphics) {
    const gl::MaterialRegistry& materials = graphics.materials();
    // Clusters by index (the layout's ids are not indices), the items' layers and records.
    std::map<std::uint32_t, std::uint32_t> clusterIndex;
    for (std::size_t index = 0; index < layout.clusters.size(); ++index) {
        clusterIndex.emplace(layout.clusters[index].id, static_cast<std::uint32_t>(index));
    }
    std::vector<scene::StaticInstance> records;
    records.reserve(layout.items.size() * kPartRoles.size());
    std::vector<Item> items;
    items.reserve(layout.items.size());
    std::vector<scene::InstanceAttributes> attributesOf;
    attributesOf.reserve(layout.items.size() * kPartRoles.size());
    std::map<std::string, std::size_t, std::less<>> byKey;
    std::map<world::PickId, std::size_t> byPick;
    bool hoveredStays = false;
    for (const world::CollectionItem& placed : layout.items) {
        const auto cluster = clusterIndex.find(placed.cluster);
        if (cluster == clusterIndex.end()) {
            return makeError(ErrorCode::ValidationFailed, "an item of a cluster the layout lacks",
                             placed.slot.toString());
        }
        // An item's own layers (their pictures kept when it stood before: I3 step 15b), else new ones.
        std::array<gl::LayerSlot, gl::kCollectionClassCount> layers{};
        for (const gl::CollectionClass cls : gl::kCollectionClasses) {
            auto slot = m_arrays.acquire(cls, cls == gl::CollectionClass::Front ? placed.frontLayerKey
                                                                                : placed.spineLayerKey);
            if (!slot) {
                return propagate(slot.error());
            }
            layers[gl::toIndex(cls)] = *slot;
        }
        Item entry;
        entry.pickId = placed.pickId;
        entry.game = placed.game;
        entry.frontKey = placed.frontLayerKey;
        entry.spineKey = placed.spineLayerKey;
        entry.transform = placed.transform;
        entry.bounds = placed.worldBounds();
        // The records name layer 0 (pending) until the item's pictures are uploaded, or the layers it holds already.
        scene::InstanceAttributes attributes = placed.attributes();
        const gl::LayerSlot& front = layers[gl::toIndex(gl::CollectionClass::Front)];
        const gl::LayerSlot& spine = layers[gl::toIndex(gl::CollectionClass::Spine)];
        if (front.resident) {
            attributes.frontLayer = front.layer;
        }
        if (spine.resident) {
            attributes.spineLayer = spine.layer;
        }
        if (m_hovered != world::kNoPick && placed.pickId == m_hovered) {
            attributes.flags |= scene::instance_flags::kHighlighted;
            hoveredStays = true;
        }
        // I3 step 13a's hidden reasons of an item that stood (pulled out, its front shown by the hero texture) carry
        // over to its new records (I3 step 15b's in-place update).
        if (const auto stood = m_byPick.find(placed.pickId); stood != m_byPick.end()) {
            entry.pulled = m_items[stood->second].pulled;
            entry.heroFront = m_items[stood->second].heroFront;
        }
        for (std::size_t part = 0; part < kPartRoles.size(); ++part) {
            if (!m_partMeshes[part]) {
                continue;
            }
            scene::MaterialHandle material = m_shellMaterial;
            if (part == static_cast<std::size_t>(Part::Front)) {
                material = m_layered[gl::toIndex(gl::CollectionClass::Front)].at(front.array);
                entry.front = records.size();
            } else if (part == static_cast<std::size_t>(Part::Spine)) {
                material = m_layered[gl::toIndex(gl::CollectionClass::Spine)].at(spine.array);
                entry.spine = records.size();
            } else {
                entry.shell = records.size();
            }
            scene::InstanceAttributes recordAttributes = attributes;
            const bool frontPart = part == static_cast<std::size_t>(Part::Front);
            if (entry.pulled || (entry.heroFront && frontPart)) {
                recordAttributes.flags |= scene::instance_flags::kHidden;
            }
            records.push_back(scene::StaticInstance{*m_partMeshes[part], material, materials.material(material).shader,
                                                    placed.transform, part == static_cast<std::size_t>(Part::Shell),
                                                    recordAttributes, cluster->second});
            attributesOf.push_back(recordAttributes);
        }
        byKey.emplace(entry.frontKey, items.size());
        byKey.emplace(entry.spineKey, items.size());
        byPick.emplace(entry.pickId, items.size());
        items.push_back(std::move(entry));
    }
    auto group = gl::StaticInstanceGroup::create(records, static_cast<std::uint32_t>(layout.clusters.size()),
                                                 graphics.meshes(), materials, "Collection " + m_room.toString());
    if (!group) {
        return propagate(group.error());
    }
    m_sources = std::move(records);
    m_group = std::move(*group);
    m_groupList[0] = &m_group;
    m_items = std::move(items);
    m_attributes = std::move(attributesOf);
    m_byKey = std::move(byKey);
    m_byPick = std::move(byPick);
    if (!hoveredStays) {
        m_hovered = world::kNoPick;
    }
    return ok();
}

Expected<CollectionUpdate> CollectionInstance::applyLayout(const world::CollectionLayout& layout,
                                                           SceneGraphics& graphics,
                                                           int uniformBufferOffsetAlignment) {
    if (layout.groups.size() != 1 || layout.groups.front().mediaFamily != m_family || layout.room != m_room) {
        return makeError(ErrorCode::Unsupported, "the collection's room or media family changed",
                         layout.room.toString());
    }
    CollectionUpdate update;
    // The layers of the games the room no longer holds are released (kept with their pictures until reused).
    std::set<catalog::GameId> standing;
    for (const world::CollectionItem& placed : layout.items) {
        standing.insert(placed.game);
    }
    for (const Item& item : m_items) {
        if (!standing.contains(item.game)) {
            m_arrays.release(gl::CollectionClass::Front, item.frontKey);
            m_arrays.release(gl::CollectionClass::Spine, item.spineKey);
            ++update.released;
        }
    }
    // Room for every item: an array smaller than the room grows by a GPU copy (its materials keep sampling it), an
    // array added gets its own layered material.
    bool arrayAdded = false;
    for (const gl::CollectionClass cls : gl::kCollectionClasses) {
        const std::size_t arrays = m_arrays.arrayCount(cls);
        const std::size_t capacity = m_arrays.itemCapacity(cls);
        if (Status reserved = m_arrays.reserve(cls, layout.items.size()); !reserved) {
            return propagate(reserved.error());
        }
        update.grown = update.grown || m_arrays.itemCapacity(cls) != capacity;
        arrayAdded = arrayAdded || m_arrays.arrayCount(cls) != arrays;
    }
    if (arrayAdded) {
        addLayeredMaterials(graphics);
        if (Status uploaded = graphics.materials().upload(uniformBufferOffsetAlignment, "MaterialUniforms");
            !uploaded) {
            return propagate(uploaded.error());
        }
        update.arraysAdded = true;
    }
    if (Status items = setItems(layout, graphics); !items) {
        return propagate(items.error());
    }
    spdlog::info("Collection of {} updated in place: {} item(s), {} released, arrays {}{} ({:.1f} MiB)",
                 layout.room.toString(), m_items.size(), update.released, update.grown ? "grown" : "as they were",
                 update.arraysAdded ? ", an array added" : "",
                 static_cast<double>(m_arrays.estimatedBytes()) / (1024.0 * 1024.0));
    return update;
}

std::optional<catalog::GameId> CollectionInstance::gameOf(world::PickId pickId) const {
    const auto found = m_byPick.find(pickId);
    return found == m_byPick.end() ? std::nullopt : std::optional<catalog::GameId>(m_items[found->second].game);
}

std::vector<core::Aabb> CollectionInstance::itemBounds() const {
    std::vector<core::Aabb> bounds;
    bounds.reserve(m_items.size());
    for (const Item& item : m_items) {
        bounds.push_back(item.bounds);
    }
    return bounds;
}

std::optional<std::size_t> CollectionInstance::itemOfPick(world::PickId pickId) const {
    const auto found = m_byPick.find(pickId);
    return found == m_byPick.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

Status CollectionInstance::setFrontHidden(std::size_t item, bool hidden) {
    if (item >= m_items.size() || !m_items[item].front) {
        return makeError(ErrorCode::InvalidArgument, "no front of that item", std::to_string(item));
    }
    m_items[item].heroFront = hidden;
    return writeHidden(item);
}

Status CollectionInstance::writeHidden(std::size_t item) {
    const Item& entry = m_items[item];
    for (const std::optional<std::size_t>& source : {entry.shell, entry.front, entry.spine}) {
        if (!source) {
            continue;
        }
        const bool hidden = entry.pulled || (entry.heroFront && source == entry.front);
        scene::InstanceAttributes attributes = m_attributes[*source];
        if (hidden) {
            attributes.flags |= scene::instance_flags::kHidden;
        } else {
            attributes.flags &= ~scene::instance_flags::kHidden;
        }
        if (Status written = writeRecord(*source, attributes); !written) {
            return written;
        }
    }
    return ok();
}

Expected<scene::RenderItem> CollectionInstance::frontRenderItem(std::size_t item,
                                                                scene::MaterialHandle material) const {
    if (item >= m_items.size() || !m_items[item].front || !m_frontMesh) {
        return makeError(ErrorCode::InvalidArgument, "no front of that item", std::to_string(item));
    }
    scene::RenderItem render;
    render.mesh = *m_frontMesh;
    render.material = material;
    render.shader = m_heroShader;
    render.model = m_items[item].transform;
    render.castsShadow = false;
    render.attributes = m_attributes[*m_items[item].front];
    render.attributes.flags &= ~scene::instance_flags::kHidden;
    return render;
}

std::optional<std::size_t> CollectionInstance::itemOfKey(const std::string& key) const {
    const auto found = m_byKey.find(key);
    return found == m_byKey.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

Status CollectionInstance::writeRecord(std::size_t source, const scene::InstanceAttributes& attributes) {
    if (m_attributes.at(source) == attributes) {
        return ok();
    }
    m_attributes[source] = attributes;
    return m_group.setAttributes(source, attributes);
}

Status CollectionInstance::layerResident(const gl::LayerUpload& upload) {
    const std::optional<std::size_t> item = itemOfKey(upload.key);
    if (!item) {
        return ok(); // a layer of no item of this room
    }
    const Item& entry = m_items[*item];
    const bool front = upload.cls == gl::CollectionClass::Front;
    const std::optional<std::size_t> source = front ? entry.front : entry.spine;
    if (!source) {
        return ok();
    }
    scene::InstanceAttributes attributes = m_attributes[*source];
    std::uint32_t& named = front ? attributes.frontLayer : attributes.spineLayer;
    // A record that names a layer already shows its item's picture: this one replaces it (I4 step 6b's count).
    ++m_uploads;
    m_reuploads += named != gl::kPendingLayer ? 1 : 0;
    named = upload.layer;
    return writeRecord(*source, attributes);
}

Status CollectionInstance::writeFlag(std::size_t item, std::uint32_t flag, bool on) {
    const Item& entry = m_items[item];
    for (const std::optional<std::size_t>& source : {entry.shell, entry.front, entry.spine}) {
        if (!source) {
            continue;
        }
        scene::InstanceAttributes attributes = m_attributes[*source];
        if (on) {
            attributes.flags |= flag;
        } else {
            attributes.flags &= ~flag;
        }
        if (Status written = writeRecord(*source, attributes); !written) {
            return written;
        }
    }
    return ok();
}

Status CollectionInstance::setHovered(world::PickId pickId) {
    if (pickId == m_hovered) {
        return ok();
    }
    if (const auto previous = m_byPick.find(m_hovered); previous != m_byPick.end()) {
        if (Status cleared = writeFlag(previous->second, scene::instance_flags::kHighlighted, false); !cleared) {
            return cleared;
        }
    }
    m_hovered = world::kNoPick;
    if (const auto next = m_byPick.find(pickId); next != m_byPick.end()) {
        if (Status set = writeFlag(next->second, scene::instance_flags::kHighlighted, true); !set) {
            return set;
        }
        m_hovered = pickId;
    }
    return ok();
}

Status CollectionInstance::setHidden(world::PickId pickId, bool hidden) {
    const auto found = m_byPick.find(pickId);
    if (found == m_byPick.end()) {
        return ok();
    }
    m_items[found->second].pulled = hidden;
    return writeHidden(found->second);
}

std::vector<world::PickId> CollectionInstance::hiddenItems() const {
    std::vector<world::PickId> hidden;
    for (const auto& [pickId, index] : m_byPick) {
        const Item& entry = m_items[index];
        for (const std::optional<std::size_t>& source : {entry.shell, entry.front, entry.spine}) {
            const bool heroShown = entry.heroFront && source == entry.front;
            if (source && !heroShown && (m_attributes[*source].flags & scene::instance_flags::kHidden) != 0) {
                hidden.push_back(pickId);
                break;
            }
        }
    }
    return hidden;
}

bool CollectionInstance::staticDrawn(world::PickId pickId) const {
    const auto found = m_byPick.find(pickId);
    if (found == m_byPick.end()) {
        return false;
    }
    const Item& entry = m_items[found->second];
    for (const std::optional<std::size_t>& source : {entry.shell, entry.front, entry.spine}) {
        if (source && (m_attributes[*source].flags & scene::instance_flags::kHidden) == 0) {
            return true;
        }
    }
    return false;
}

void CollectionInstance::appendRenderItems(world::PickId pickId, const glm::mat4& model,
                                           std::vector<scene::RenderItem>& out) const {
    const auto found = m_byPick.find(pickId);
    if (found == m_byPick.end()) {
        return;
    }
    const Item& entry = m_items[found->second];
    for (const std::optional<std::size_t>& source : {entry.shell, entry.front, entry.spine}) {
        if (!source) {
            continue;
        }
        const scene::StaticInstance& part = m_sources[*source];
        scene::InstanceAttributes attributes = m_attributes[*source];
        attributes.flags &= ~scene::instance_flags::kHidden;
        if (entry.heroFront && source == entry.front) {
            // The hero texture's front (I3 step 12) travels with the pulled item.
            out.push_back(scene::RenderItem{part.mesh, m_heroMaterial, m_heroShader, model, false, attributes});
        } else {
            out.push_back(
                scene::RenderItem{part.mesh, part.material, part.shader, model, part.castsShadow, attributes});
        }
    }
}

} // namespace rv::app
