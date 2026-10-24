#include <rv/app/scene_instance.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// Logs the lights left out and the lights block; `area` names the area whose lights it holds (empty: the whole scene).
void logLights(std::string_view scene, std::string_view area, const gl::LightsUniforms& block,
               std::span<const gl::DroppedLight> dropped, const scene::EnvironmentSetup& environment) {
    for (const gl::DroppedLight& light : dropped) {
        spdlog::warn("Scene '{}': light '{}' is not drawn: {}", scene, light.name, light.reason);
    }
    const std::string whose = area.empty() ? std::string("Scene lights") : fmt::format("Lights of the area '{}'", area);
    spdlog::info("{}: {} point/spot, {} directional; ambient intensity {:g}; exposure {:g}, bloom strength {:g}, bloom "
                 "radius {:g}, vignette {:g}",
                 whose, block.punctualLightCount, block.directionalIntensity > 0.0f ? 1 : 0, block.ambientIntensity,
                 environment.exposure, environment.bloomStrength, environment.bloomRadius, environment.vignette);
}

/// Logs the key light of `packed` (made from `lights`) and its shadow, or why there is none; `area` names the area
/// whose lights were packed (empty: the whole scene).
void logKeyLight(std::string_view scene, std::string_view area, std::span<const scene::Light> lights,
                 const gl::PackedLights& packed) {
    const std::string where =
        area.empty() ? fmt::format("Scene '{}'", scene) : fmt::format("Scene '{}', area '{}'", scene, area);
    const gl::ShadowLight& key = packed.shadowLight;
    if (key.casts() && packed.keyLight && *packed.keyLight < lights.size()) {
        spdlog::info("{}: key light '{}' ({}) casts the shadows, strength {:g}, {}x{} shadow map", where,
                     gl::lightName(lights[*packed.keyLight], *packed.keyLight), scene::toString(key.type),
                     key.strength, gl::kShadowMapResolution, gl::kShadowMapResolution);
    } else if (!packed.shadowNote.empty()) {
        spdlog::warn("{}: no shadows: {}", where, packed.shadowNote);
    } else {
        spdlog::info("{}: no light casts shadows", where);
    }
}

/// Logs the key light of every area (its first light with castsShadows) as when the eye is in that area: what it
/// casts, or why it casts nothing (a point light, a dropped one, a directional light without casters). A view packs an
/// area's directional lights and its key light before its other lights (scene::AreaLighting), so packing those alone
/// gives the key light's fate in every view from the area.
void logAreaKeyLights(std::string_view scene, std::span<const scene::SceneArea> areas,
                      std::span<const scene::Light> lights, const scene::EnvironmentSetup& environment,
                      const core::Aabb& casterBounds) {
    const std::vector<std::vector<std::uint32_t>> byArea = scene::lightsByArea(lights, areas);
    std::vector<std::uint32_t> selection;
    for (std::size_t area = 0; area < areas.size(); ++area) {
        selection.clear();
        bool keySeen = false;
        for (const std::uint32_t index : byArea[area]) {
            const scene::Light& light = lights[index];
            if ((light.castsShadows && !keySeen) || light.type == scene::LightType::Directional) {
                selection.push_back(index);
            }
            keySeen = keySeen || light.castsShadows;
        }
        gl::PackedLights packed = gl::packLights(lights, selection, environment);
        gl::requireShadowCasters(packed, lights, casterBounds);
        logKeyLight(scene, areas[area].id, lights, packed);
    }
}

/// Logs the ambient occlusion.
void logAmbientOcclusion(std::string_view scene, const scene::SsaoSetup& ssao) {
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
        for (const scene::SceneArea& area : instance.m_areas) {
            instance.m_areaEnvironments.push_back(area.environment.over(description.environment));
        }
        spdlog::info("Scene '{}': {} area(s); the views pack the lights of the area that holds the eye, '{}' at the "
                     "start",
                     description.name, instance.m_areas.size(), instance.areaId());
    }
    instance.applyLighting();
    logLights(description.name, instance.areaId(), instance.m_lights, instance.m_droppedLights,
              instance.viewEnvironment());
    if (instance.m_areas.empty()) {
        gl::PackedLights packed;
        packed.shadowLight = instance.m_shadowLight;
        packed.keyLight = instance.m_keyLight;
        packed.shadowNote = instance.m_shadowNote;
        logKeyLight(description.name, {}, instance.m_sceneLights, packed);
    } else {
        // Every area's key light, not only the start area's: a view shadow-maps the key light of the eye's area.
        logAreaKeyLights(description.name, instance.m_areas, instance.m_sceneLights, instance.m_environment,
                         instance.m_shadowCasterBounds);
    }
    logAmbientOcclusion(description.name, instance.m_environment.ssao);
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

namespace {

/// `from`'s area terms (exposure, ambient, fill: what an area may set) into `to`.
void copyAreaTerms(const scene::EnvironmentSetup& from, scene::EnvironmentSetup& to) noexcept {
    to.exposure = from.exposure;
    to.ambientSky = from.ambientSky;
    to.ambientGround = from.ambientGround;
    to.ambientIntensity = from.ambientIntensity;
    to.fillColor = from.fillColor;
    to.fillIntensity = from.fillIntensity;
}

} // namespace

void SceneInstance::applyLighting() {
    if (!m_areaLighting.empty()) {
        (void)takeEnvironmentEdit();
        (void)m_areaLighting.reselect(m_sceneLights, m_eye, m_forward);
        m_environmentEase.snap(areaEnvironment());
        (void)advanceShadow(0.0f, true);
    }
    packLighting();
}

scene::EnvironmentSetup SceneInstance::areaEnvironment() const {
    scene::EnvironmentSetup result = m_environment;
    const auto area = m_areaLighting.area();
    if (area && *area < m_areaEnvironments.size()) {
        copyAreaTerms(m_areaEnvironments[*area], result);
    }
    return result;
}

bool SceneInstance::takeEnvironmentEdit() {
    const auto area = m_areaLighting.area();
    if (!area || *area >= m_areaEnvironments.size()) {
        return false;
    }
    bool edited = false;
    if (m_shownArea && *m_shownArea < m_areaEnvironments.size() && !scene::sameAreaTerms(m_environment, m_shown)) {
        // The overlay edited the area terms it shows: those of the area it showed them for.
        copyAreaTerms(m_environment, m_areaEnvironments[*m_shownArea]);
        edited = *m_shownArea == *area;
    }
    if (m_shownArea != area) {
        copyAreaTerms(m_areaEnvironments[*area], m_environment);
        m_shownArea = area;
    }
    m_shown = m_environment;
    if (edited) {
        m_environmentEase.snap(areaEnvironment());
    }
    return edited;
}

scene::EnvironmentSetup SceneInstance::viewEnvironment() const {
    scene::EnvironmentSetup view = m_environment;
    if (m_areaLighting.empty()) {
        return view;
    }
    copyAreaTerms(m_environmentEase.current(), view);
    return view;
}

bool SceneInstance::advanceEnvironment(float seconds) {
    if (m_areaLighting.empty()) {
        return false;
    }
    bool changed = takeEnvironmentEdit();
    if (!m_environmentEase.easing()) {
        // An edit of a term no area sets (the developer overlay).
        const scene::EnvironmentSetup target = areaEnvironment();
        if (!scene::sameAreaTerms(target, m_environmentEase.target())) {
            m_environmentEase.snap(target);
            changed = true;
        }
    }
    changed = m_environmentEase.advance(seconds) || changed;
    if (changed) {
        gl::packAmbient(m_lights, viewEnvironment());
    }
    return changed;
}

bool SceneInstance::advanceShadow(float seconds, bool instant) {
    const std::optional<std::uint32_t> wanted = m_areaLighting.keyLight();
    const std::optional<std::uint32_t> source = m_shadowSource;
    const float progress = m_shadowProgress;
    const float step = std::isfinite(seconds) && seconds > 0.0f ? seconds / scene::kLightFadeSeconds : 0.0f;
    const std::span<const std::uint32_t> selection = m_areaLighting.selection();
    const bool sourcePacked =
        m_shadowSource && std::find(selection.begin(), selection.end(), *m_shadowSource) != selection.end();
    if (instant) {
        m_shadowSource = wanted;
        m_shadowProgress = wanted ? 1.0f : 0.0f;
    } else if (m_shadowSource == wanted) {
        m_shadowProgress = std::min(1.0f, m_shadowProgress + step);
    } else if (sourcePacked && m_shadowProgress > 0.0f) {
        // The shadow of the key light the eye left fades out before the next one's fades in.
        m_shadowProgress = std::max(0.0f, m_shadowProgress - step);
        if (m_shadowProgress <= 0.0f) {
            m_shadowSource = wanted;
        }
    } else {
        m_shadowSource = wanted;
        m_shadowProgress = 0.0f;
    }
    return m_shadowSource != source || m_shadowProgress != progress;
}

void SceneInstance::packLighting() {
    gl::PackedLights packed;
    if (m_areaLighting.empty()) {
        packed = gl::packLights(m_sceneLights, m_environment);
    } else {
        gl::LightSelection selection;
        selection.indices = m_areaLighting.selection();
        selection.weights = m_areaLighting.weights();
        selection.portals = m_areaLighting.portals();
        // The light whose shadow the views draw (advanceShadow()): the eye's area's key light, or the one it left
        // while its shadow fades out.
        selection.keyCandidates = 0;
        if (m_shadowSource) {
            const auto found = std::find(selection.indices.begin(), selection.indices.end(), *m_shadowSource);
            if (found != selection.indices.end()) {
                selection.key = static_cast<std::size_t>(found - selection.indices.begin());
            }
        }
        const float t = m_shadowProgress;
        selection.keyShadowWeight = t * t * (3.0f - 2.0f * t);
        packed = gl::packLights(m_sceneLights, selection, viewEnvironment());
    }
    gl::requireShadowCasters(packed, m_sceneLights, m_shadowCasterBounds);
    m_lights = packed.uniforms;
    m_packedLights = std::move(packed.punctual);
    m_droppedLights = std::move(packed.dropped);
    m_shadowLight = packed.shadowLight;
    m_keyLight = packed.keyLight;
    m_shadowNote = std::move(packed.shadowNote);
}

std::string_view SceneInstance::areaId() const noexcept {
    const auto area = m_areaLighting.area();
    return area && *area < m_areas.size() ? std::string_view(m_areas[*area].id) : std::string_view();
}

bool SceneInstance::followEye(const glm::vec3& eye, const glm::vec3& forward, float seconds) {
    if (m_areaLighting.empty()) {
        return false;
    }
    m_eye = eye;
    m_forward = forward;
    // The first eye followed is where the views begin, and a teleport cuts to another place: its area's environment
    // and lights at once, never eased from the scene camera's area or the place left.
    const bool instant = !m_eyeFollowed || m_reduceMotion || m_snapNext;
    m_eyeFollowed = true;
    m_snapNext = false;
    const auto before = m_areaLighting.area();
    bool changed = m_areaLighting.update(m_areas, m_sceneLights, eye, forward, seconds, instant);
    const bool areaChanged = before != m_areaLighting.area();
    if (instant) {
        m_environmentEase.snap(areaEnvironment());
    } else if (areaChanged) {
        // The views ease to the new area's environment (advanceEnvironment()).
        m_environmentEase.retarget(areaEnvironment(), false);
    }
    changed = advanceShadow(seconds, instant) || changed;
    if (changed) {
        packLighting();
    }
    if (areaChanged) {
        // What each area's key light casts was logged at load (logAreaKeyLights).
        spdlog::debug("Scene '{}': the eye entered the area '{}': {} light(s) packed", m_name, areaId(),
                      m_lights.punctualLightCount);
    }
    return areaChanged;
}

} // namespace rv::app
