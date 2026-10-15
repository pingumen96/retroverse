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

namespace rv::app {
namespace {

/// Base color texture of a material instance from its picture (PlannedMaterial::picture): an asset file or a pattern
/// generated at the planned size, both clamped to their edges (single pictures).
const gl::Texture2D& pictureTexture(const PlannedMaterial& planned, gl::TextureCache& textures) {
    const scene::TextureSource& source = *planned.picture;
    if (source.kind == scene::TextureSource::Kind::File) {
        return textures.file(source.path, gl::ColorSpace::Srgb, gl::TextureWrap::ClampToEdge);
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

    for (const scene::RenderItem& item : instance.m_items) {
        if (item.castsShadow) {
            const core::Aabb bounds = resources.meshes.bounds(item.mesh);
            if (!bounds.isEmpty()) {
                instance.m_shadowCasterBounds.expand(bounds.transformed(item.model));
            }
        }
    }

    instance.m_sceneLights = description.lights;
    instance.m_environment = description.environment;
    instance.applyLighting();
    logLights(description.name, instance.m_lights, instance.m_droppedLights, instance.m_environment);
    logShadows(description.name, instance.m_sceneLights, instance.m_shadowLight, instance.m_shadowNote,
               instance.m_environment.ssao);
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
    gl::PackedLights packed = gl::packLights(m_sceneLights, m_environment);
    m_lights = packed.uniforms;
    m_droppedLights = std::move(packed.dropped);
    m_shadowLight = packed.shadowLight;
    m_shadowNote = std::move(packed.shadowNote);
}

} // namespace rv::app
