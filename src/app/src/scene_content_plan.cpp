#include <rv/app/scene_content_plan.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

#include <fmt/format.h>

#include <rv/assets/test_patterns.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/render/material_uniforms.hpp>

namespace rv::app {
namespace {

using Index = std::map<std::string, std::size_t, std::less<>>;

/// Index of `key` in `index`, adding `make()` to `entries` at first use.
template <class Entry, class Make>
std::size_t findOrAdd(Index& index, std::vector<Entry>& entries, const std::string& key, const Make& make) {
    if (const auto found = index.find(key); found != index.end()) {
        return found->second;
    }
    entries.push_back(make());
    return index.emplace(key, entries.size() - 1).first->second;
}

/// Pattern size for `part` drawn with `model`: the aspect of its surface as drawn, about assets::kPatternPixelsPerMeter
/// pixels per meter of it.
glm::ivec2 patternSizeOf(const scene::ScenePart& part, const glm::mat4& model) {
    const glm::vec2 metersPerUnit = geometry::uvMetersPerUnit(*part.mesh, model);
    const float aspect = metersPerUnit.y > 0.0f ? metersPerUnit.x / metersPerUnit.y : 1.0f;
    return assets::patternSizeForAspect(aspect,
                                        assets::patternLongSideFor(std::max(metersPerUnit.x, metersPerUnit.y)));
}

class MaterialPlanner {
public:
    explicit MaterialPlanner(std::vector<PlannedMaterial>& materials) : m_materials(materials) {}

    /// Index of the material `part` of `object` is drawn with at `model`, added at first use.
    std::size_t materialOf(const scene::SceneObjectGeometry& object, const scene::ScenePart& part,
                           const glm::mat4& model) {
        if (!part.texture) {
            return findOrAdd(m_shared, m_materials, part.material.name,
                             [&] { return makeMaterial(part.material.name, part.material.name, part.material); });
        }
        std::string key = object.id + "/" + part.role;
        glm::ivec2 patternSize(0);
        if (part.texture->kind == scene::TextureSource::Kind::Pattern) {
            patternSize = patternSizeOf(part, model);
            key += fmt::format("@{}x{}", patternSize.x, patternSize.y);
        }
        return findOrAdd(m_instances, m_materials, key, [&] {
            PlannedMaterial material =
                makeMaterial(key, fmt::format("{} ({})", key, part.material.name), part.material);
            material.picture = part.texture;
            material.patternSize = patternSize;
            return material;
        });
    }

private:
    static PlannedMaterial makeMaterial(std::string key, std::string label, const assets::MaterialDesc& desc) {
        PlannedMaterial material;
        material.key = std::move(key);
        material.label = std::move(label);
        material.material = desc;
        material.shader = gl::shaderKindOf(desc);
        return material;
    }

    std::vector<PlannedMaterial>& m_materials;
    // Separate key spaces: a material name can never be mistaken for an instance key.
    Index m_shared;
    Index m_instances;
};

} // namespace

std::size_t SceneContentPlan::pictureMaterialCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(materials.begin(), materials.end(),
                                                  [](const PlannedMaterial& m) { return m.picture.has_value(); }));
}

SceneContentPlan planSceneContent(const scene::SceneGeometry& geometry) {
    SceneContentPlan plan;
    MaterialPlanner materials(plan.materials);
    Index meshes;
    for (std::size_t objectIndex = 0; objectIndex < geometry.objects.size(); ++objectIndex) {
        const scene::SceneObjectGeometry& object = geometry.objects[objectIndex];
        for (std::size_t partIndex = 0; partIndex < object.parts.size(); ++partIndex) {
            const scene::ScenePart& part = object.parts[partIndex];
            if (!part.mesh) {
                continue;
            }
            const std::size_t mesh =
                findOrAdd(meshes, plan.meshes, part.meshKey, [&] { return PlannedMesh{part.meshKey, part.mesh}; });
            for (const glm::mat4& model : object.instances) {
                plan.items.push_back(PlannedItem{objectIndex, partIndex, mesh,
                                                 materials.materialOf(object, part, model), model,
                                                 object.castsShadows});
            }
        }
        if (object.model) {
            for (const glm::mat4& model : object.instances) {
                plan.modelInstances.push_back(PlannedModelInstance{objectIndex, model, object.castsShadows});
            }
        }
    }
    return plan;
}

} // namespace rv::app
