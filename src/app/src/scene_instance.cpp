#include <rv/app/scene_instance.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/scene_content_plan.hpp>
#include <rv/render/gpu_model.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/texture_cache.hpp>
#include <rv/render/tonemap.hpp>

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

void logLights(const scene::SceneDescription& scene, const gl::PackedLights& packed) {
    for (const gl::DroppedLight& dropped : packed.dropped) {
        spdlog::warn("Scene '{}': light '{}' is not drawn: {}", scene.name, dropped.name, dropped.reason);
    }
    const gl::LightsUniforms& block = packed.uniforms;
    spdlog::info("Scene lights: {} point/spot, {} directional; ambient intensity {:g}, exposure {:g}",
                 block.punctualLightCount, block.directionalIntensity > 0.0f ? 1 : 0, block.ambientIntensity,
                 block.exposure);
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

    const gl::PackedLights lights = gl::packLights(description.lights, description.environment);
    logLights(description, lights);
    instance.m_lights = lights.uniforms;
    // The background is scene radiance like any surface: exposure and tonemapping apply before the sRGB encoding.
    instance.m_clearColor =
        glm::vec4(gl::tonemap(description.environment.clearColor, description.environment.exposure), 1.0f);
    instance.m_collision = scene::buildCollisionWorld(geometry);
    scene::addSceneDebugLines(geometry, instance.m_debugLines);

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

} // namespace rv::app
