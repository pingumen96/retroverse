#include <rv/app/scene_instance.hpp>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/scene_content_plan.hpp>
#include <rv/render/gpu_model.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/scene/batch_planner.hpp>

namespace rv::app {
namespace {

/// Base color texture of a material instance from its picture (PlannedMaterial::picture): an asset file or a pattern
/// generated at the planned size, both clamped to their edges (single pictures), or a procedural surface texture,
/// repeating.
const gl::Texture2D& pictureTexture(const PlannedMaterial& planned, gl::TextureCache& textures) {
    const scene::TextureSource& source = *planned.picture;
    switch (source.kind) {
    case scene::TextureSource::Kind::File:
        return textures.file(source.path, gl::ColorSpace::Srgb, gl::TextureWrap::ClampToEdge);
    case scene::TextureSource::Kind::Procedural:
        return textures.procedural(source.procedural, gl::ColorSpace::Srgb);
    case scene::TextureSource::Kind::Pattern:
        break;
    }
    return textures.pattern(source.pattern, source.title, source.seed, planned.patternSize, gl::ColorSpace::Srgb);
}

void logLights(std::string_view scene, const gl::LightsUniforms& block, std::span<const gl::DroppedLight> dropped,
               const scene::EnvironmentSetup& environment) {
    for (const gl::DroppedLight& light : dropped) {
        spdlog::warn("Scene '{}': light '{}' is not drawn: {}", scene, light.name, light.reason);
    }
    spdlog::info("Scene lights: {} point/spot, {} directional; ambient intensity {:g}; exposure {:g}, bloom strength "
                 "{:g}, bloom radius {:g}, vignette {:g}",
                 block.punctualLightCount, block.directionalIntensity > 0.0f ? 1 : 0, block.ambientIntensity,
                 environment.exposure, environment.bloomStrength, environment.bloomRadius, environment.vignette);
}

/// Logs the key light (the first light with castsShadows) and its shadow, or why there is none, and the ambient
/// occlusion.
void logShadows(std::string_view scene, std::span<const scene::Light> lights, const gl::ShadowLight& key,
                std::string_view note, const scene::SsaoSetup& ssao) {
    const auto first =
        std::find_if(lights.begin(), lights.end(), [](const scene::Light& light) { return light.castsShadows; });
    if (key.casts() && first != lights.end()) {
        const std::string name =
            first->name.empty() ? fmt::format("#{}", std::distance(lights.begin(), first)) : first->name;
        spdlog::info("Scene '{}': key light '{}' ({}) casts the shadows, strength {:g}, {}x{} shadow map", scene,
                     name, scene::toString(key.type), key.strength, gl::kShadowMapResolution,
                     gl::kShadowMapResolution);
    } else if (!note.empty()) {
        spdlog::warn("Scene '{}': no shadows: {}", scene, note);
    } else {
        spdlog::info("Scene '{}': no light casts shadows", scene);
    }
    if (ssao.enabled) {
        spdlog::info("Scene '{}': ambient occlusion radius {:g} m, intensity {:g}", scene, ssao.radius,
                     ssao.intensity);
    } else {
        spdlog::info("Scene '{}': ambient occlusion off", scene);
    }
}

} // namespace

Expected<SceneInstance> SceneInstance::create(const scene::SceneDescription& description,
                                              const assets::MaterialLibrary& library,
                                              const scene::ModelLoader& loadModel, const SceneResources& resources) {
    const scene::SceneGeometry geometry = scene::buildSceneGeometry(description, library, loadModel);
    const SceneContentPlan plan = planSceneContent(geometry);

    SceneInstance instance;
    instance.m_name = description.name;

    // One upload per distinct mesh; a mesh that cannot be uploaded leaves its parts undrawn.
    const std::size_t meshesBefore = resources.meshes.size();
    std::vector<std::optional<scene::MeshHandle>> meshes(plan.meshes.size());
    for (std::size_t index = 0; index < plan.meshes.size(); ++index) {
        const PlannedMesh& planned = plan.meshes[index];
        auto handle = resources.meshes.add(planned.key, *planned.mesh);
        if (handle) {
            meshes[index] = *handle;
        } else {
            spdlog::error("Mesh '{}' cannot be uploaded and its parts are not drawn: {}", planned.key,
                          handle.error().toString());
        }
    }

    const std::size_t materialsBefore = resources.materials.size();
    std::vector<scene::MaterialHandle> materials;
    materials.reserve(plan.materials.size());
    for (const PlannedMaterial& planned : plan.materials) {
        gl::MaterialTextures slots = gl::resolveMaterialTextures(planned.material, resources.textures);
        if (planned.picture) {
            slots[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] = &pictureTexture(planned, resources.textures);
        }
        materials.push_back(resources.materials.add(planned.key, planned.label, planned.material, slots));
    }

    instance.m_items.reserve(plan.items.size());
    for (const PlannedItem& item : plan.items) {
        if (meshes[item.mesh]) {
            const scene::MaterialHandle material = materials[item.material];
            instance.m_items.push_back(scene::RenderItem{*meshes[item.mesh], material,
                                                         resources.materials.material(material).shader, item.model,
                                                         item.castsShadow});
        }
    }
    // One GpuModel per distinct model; each model instance adds the model's primitives.
    std::map<std::string, gl::GpuModel, std::less<>> models;
    for (const PlannedModelInstance& planned : plan.modelInstances) {
        const scene::SceneObjectGeometry& object = geometry.objects[planned.object];
        auto model = models.find(object.modelPath);
        if (model == models.end()) {
            model =
                models
                    .emplace(object.modelPath, gl::GpuModel::create(object.modelPath, *object.model, resources.meshes,
                                                                    resources.materials, resources.textures))
                    .first;
        }
        model->second.appendRenderItems(planned.model, planned.castsShadow, instance.m_items);
    }
    if (resources.materials.size() > 0) {
        if (auto uploaded = resources.materials.upload(resources.uniformBufferOffsetAlignment, "MaterialUniforms");
            !uploaded) {
            return propagate(uploaded.error());
        }
    }

    // What the shadow pass draws (gl::SceneRenderer plans it like this): blended items cast no shadow.
    scene::PlanOptions casters;
    casters.localBounds = [&resources](scene::MeshHandle mesh) {
        return resources.meshes.bounds(mesh);
    };
    casters.passOf = [&resources](scene::MaterialHandle material) {
        return resources.materials.pass(material);
    };
    instance.m_shadowCasterBounds = scene::shadowCasterBounds(instance.m_items, casters);

    instance.m_sceneLights = description.lights;
    instance.m_environment = description.environment;
    if (!description.areas.empty()) {
        // The area of the scene camera's eye until the application follows the walker's (followEye()).
        instance.m_areas = description.areas;
        instance.m_eye = description.camera.position;
        instance.m_areaLighting = scene::AreaLighting(instance.m_areas, instance.m_sceneLights, instance.m_eye,
                                                      gl::kMaxPunctualLights);
        spdlog::info("Scene '{}': {} area(s); the views pack the lights of the area that holds the eye, '{}' at the "
                     "start",
                     description.name, instance.m_areas.size(), instance.areaId());
    }
    instance.applyLighting();
    logLights(description.name, instance.m_lights, instance.m_droppedLights, instance.m_environment);
    if (instance.m_areas.empty()) {
        logShadows(description.name, instance.m_sceneLights, instance.m_shadowLight, instance.m_shadowNote,
                   instance.m_environment.ssao);
    } else {
        std::vector<scene::Light> packed;
        for (const std::uint32_t index : instance.areaSelection()) {
            packed.push_back(instance.m_sceneLights[index]);
        }
        logShadows(description.name, packed, instance.m_shadowLight, instance.m_shadowNote,
                   instance.m_environment.ssao);
    }
    instance.m_collision = scene::buildCollisionWorld(geometry);
    scene::addSceneBoundsLines(geometry, instance.m_boundsLines);
    scene::addSceneSocketLines(geometry, instance.m_socketLines);
    instance.m_objectBounds.reserve(geometry.objects.size());
    for (const scene::SceneObjectGeometry& object : geometry.objects) {
        instance.m_objectBounds.push_back(object.bounds);
    }

    spdlog::info("Scene '{}': {} object(s) ({} with a glTF model), {} instance(s), {} part(s), {} placeholder(s), {} "
                 "part(s) with the fallback material; {} triangle(s) per view",
                 description.name, geometry.objects.size(), geometry.modelObjectCount(), geometry.instanceCount(),
                 geometry.partCount(), geometry.placeholderCount(), geometry.fallbackMaterialCount(),
                 geometry.drawnTriangleCount());
    spdlog::info("Scene '{}' on the GPU: {} mesh(es) uploaded ({} distinct in the scene), {} material(s) ({} textured "
                 "instance(s)), {} render item(s); textures: {} ({:.1f} MiB), {} unavailable",
                 description.name, resources.meshes.size() - meshesBefore, geometry.uniqueMeshCount(),
                 resources.materials.size() - materialsBefore, plan.pictureMaterialCount(), instance.m_items.size(),
                 resources.textures.textureCount(),
                 static_cast<double>(resources.textures.estimatedBytes()) / (1024.0 * 1024.0),
                 resources.textures.failedCount());
    return instance;
}

void SceneInstance::applyLighting() {
    if (!m_areaLighting.empty()) {
        (void)m_areaLighting.reselect(m_sceneLights, m_eye);
    }
    packLighting();
}

void SceneInstance::packLighting() {
    gl::PackedLights packed = m_areaLighting.empty()
                                  ? gl::packLights(m_sceneLights, m_environment)
                                  : gl::packLights(m_sceneLights, m_areaLighting.selection(), m_environment);
    gl::requireShadowCasters(packed, m_sceneLights, m_shadowCasterBounds);
    m_lights = packed.uniforms;
    m_droppedLights = std::move(packed.dropped);
    m_shadowLight = packed.shadowLight;
    m_shadowNote = std::move(packed.shadowNote);
}

std::string_view SceneInstance::areaId() const noexcept {
    const auto area = m_areaLighting.area();
    return area && *area < m_areas.size() ? std::string_view(m_areas[*area].id) : std::string_view();
}

bool SceneInstance::followEye(const glm::vec3& eye) {
    if (m_areaLighting.empty()) {
        return false;
    }
    m_eye = eye;
    const auto before = m_areaLighting.area();
    if (!m_areaLighting.update(m_areas, m_sceneLights, eye)) {
        return false;
    }
    packLighting();
    const bool areaChanged = before != m_areaLighting.area();
    if (areaChanged) {
        spdlog::debug("Scene '{}': the eye entered the area '{}': {} light(s) packed, {}", m_name, areaId(),
                      m_lights.punctualLightCount, m_shadowLight.casts() ? "its key light casts the shadows"
                                                                         : "no shadow");
    }
    return areaChanged;
}

} // namespace rv::app
