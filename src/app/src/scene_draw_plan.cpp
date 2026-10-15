#include <rv/app/scene_draw_plan.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include <rv/assets/test_patterns.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/render/material_uniforms.hpp>

namespace rv::app {
namespace {

/// Pattern size for `part` drawn with `model`.
glm::ivec2 patternSizeOf(const scene::ScenePart& part, const glm::mat4& model) {
    const glm::vec2 metersPerUnit = geometry::uvMetersPerUnit(part.mesh, model);
    const float aspect = metersPerUnit.y > 0.0f ? metersPerUnit.x / metersPerUnit.y : 1.0f;
    return assets::patternSizeForAspect(aspect);
}

class MaterialPlanner {
public:
    explicit MaterialPlanner(std::vector<PlannedMaterial>& materials) : m_materials(materials) {}

    /// Index of the material `part` of `object` is drawn with at `model`, added at first use.
    std::size_t materialOf(const scene::SceneObjectGeometry& object, const scene::ScenePart& part,
                           const glm::mat4& model) {
        if (!part.texture) {
            return find(m_shared, part.material.name,
                        [&] { return makeMaterial(part.material.name, part.material.name, part.material); });
        }
        std::string key = object.id + "/" + part.role;
        glm::ivec2 patternSize(0);
        if (part.texture->kind == scene::TextureSource::Kind::Pattern) {
            patternSize = patternSizeOf(part, model);
            key += fmt::format("@{}x{}", patternSize.x, patternSize.y);
        }
        return find(m_instances, key, [&] {
            PlannedMaterial material =
                makeMaterial(key, fmt::format("{} ({})", key, part.material.name), part.material);
            material.picture = part.texture;
            material.patternSize = patternSize;
            return material;
        });
    }

private:
    using Index = std::map<std::string, std::size_t, std::less<>>;

    static PlannedMaterial makeMaterial(std::string key, std::string label, const assets::MaterialDesc& desc) {
        PlannedMaterial material;
        material.key = std::move(key);
        material.label = std::move(label);
        material.material = desc;
        material.shader = gl::shaderKindOf(desc);
        material.pass = drawPassOf(desc.alphaMode);
        return material;
    }

    template <class Make>
    std::size_t find(Index& index, const std::string& key, const Make& make) {
        if (const auto found = index.find(key); found != index.end()) {
            return found->second;
        }
        m_materials.push_back(make());
        return index.emplace(key, m_materials.size() - 1).first->second;
    }

    std::vector<PlannedMaterial>& m_materials;
    // Separate key spaces: a material name can never be mistaken for an instance key.
    Index m_shared;
    Index m_instances;
};

} // namespace

DrawPass drawPassOf(assets::AlphaMode mode) noexcept {
    switch (mode) {
    case assets::AlphaMode::Opaque:
        return DrawPass::Opaque;
    case assets::AlphaMode::Mask:
        return DrawPass::Mask;
    case assets::AlphaMode::Blend:
        return DrawPass::Blend;
    }
    return DrawPass::Opaque;
}

std::size_t SceneDrawPlan::pictureMaterialCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(materials.begin(), materials.end(),
                                                  [](const PlannedMaterial& m) { return m.picture.has_value(); }));
}

SceneDrawPlan planSceneDraws(const scene::SceneGeometry& geometry) {
    SceneDrawPlan plan;
    MaterialPlanner materials(plan.materials);
    for (std::size_t objectIndex = 0; objectIndex < geometry.objects.size(); ++objectIndex) {
        const scene::SceneObjectGeometry& object = geometry.objects[objectIndex];
        for (std::size_t partIndex = 0; partIndex < object.parts.size(); ++partIndex) {
            for (const glm::mat4& model : object.instances) {
                plan.draws.push_back(PlannedDraw{objectIndex, partIndex,
                                                 materials.materialOf(object, object.parts[partIndex], model), model});
            }
        }
    }

    std::stable_sort(plan.draws.begin(), plan.draws.end(), [&plan](const PlannedDraw& a, const PlannedDraw& b) {
        const PlannedMaterial& left = plan.materials[a.material];
        const PlannedMaterial& right = plan.materials[b.material];
        if (left.pass != right.pass || left.pass == DrawPass::Blend) {
            return left.pass < right.pass;
        }
        return std::tuple(left.shader, a.material, a.object, a.part) <
               std::tuple(right.shader, b.material, b.object, b.part);
    });
    return plan;
}

} // namespace rv::app
